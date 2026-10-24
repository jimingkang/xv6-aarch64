// Block device layer: device table and block <-> sector translation.
//
// Drivers register whole disks with add_disk(); the block layer scans the
// partition table itself and adds the partitions (Linux add_disk ->
// blk_add_partitions -> add_partition):
//   dev 1  (unused; /dev/root names one of the devices below)
//   dev 2  mmcblk0     the whole SD card, add_disk(minors 5) from rootdev.c
//   dev 3+ mmcblk0pN   primary partition N, added by the MBR scan here
// and do_mounts.c adds two RAM disks for the temporary filesystems:
//   dev 7  ram0        rootfs, the first "/" (compare Linux rootfs)
//   dev 8  ram1        devtmpfs, the kernel-maintained /dev
// Every buffer-cache miss and write-back goes through blk_rw().
//
// Block devices are also visible as device files (major BLOCKDEV, minor =
// device number): /dev/mmcblk0, /dev/mmcblk0p2, /dev/root.  They read
// through the buffer cache and are read-only from user space.

#include <kernel/types.h>
#include "aarch64.h"
#include <kernel/param.h>
#include <kernel/spinlock.h>
#include <kernel/sleeplock.h>
#include <kernel/fs.h>
#include <kernel/buf.h>
#include <kernel/defs.h>
#include <kernel/blkdev.h>
#include <kernel/file.h>
#include <kernel/device.h>
#include <kernel/vfs.h>
#include <kernel/module.h>

static struct {
  struct spinlock lock;     // protects registration; lookups after boot are
  struct blkdev dev[NBLKDEV];
  uint reads, writes;       // device transfers (cache misses / write-backs)
} blk;

static struct file_operations blkfile_fops;

// Linux genhd_device_init(): the block layer is up before any driver probes.
void
blkdev_init(void)
{
  initlock(&blk.lock, "blkdev");
  register_chrdev(BLOCKDEV, "block", &blkfile_fops);
}
subsys_initcall("block", blkdev_init);

// Put one device into the table and the device model.  Internal: drivers
// call add_disk(), and partitions come from add_partition().  A device number
// that is already taken is refused (Linux: -EBUSY).
static struct blkdev*
blkdev_add(int dev, const char *name, uint32 start, uint32 nsect,
           uint32 bsize, blk_rw_fn rw, void *priv, struct blkdev *disk,
           int partno, int minors)
{
  if(rw == 0 || bsize < BLK_SECTOR || bsize > BLK_MAX_BSIZE ||
     bsize % BLK_SECTOR)
    return 0;
  acquire(&blk.lock);
  struct blkdev *bd = 0;
  for(int i = 0; i < NBLKDEV; i++)
    if(blk.dev[i].used && blk.dev[i].dev == dev){
      release(&blk.lock);
      printf("blkdev: %s: device %d already used by %s\n",
             name, dev, blk.dev[i].name);
      return 0;
    }
  for(int i = 0; i < NBLKDEV && bd == 0; i++)
    if(!blk.dev[i].used)
      bd = &blk.dev[i];
  if(bd == 0){
    release(&blk.lock);
    return 0;
  }
  bd->dev = dev;
  safestrcpy(bd->name, name, sizeof(bd->name));
  bd->start = start;
  bd->nsect = nsect;
  bd->bsize = bsize;
  bd->rw = rw;
  bd->priv = priv;
  bd->disk = disk;
  bd->partno = partno;
  bd->minors = minors;
  bd->ptype = 0;
  bd->uuid[0] = 0;
  bd->inflight = 0;
  bd->openers = 0;
  bd->dying = 0;
  bd->used = 1;
  release(&blk.lock);

  memset(&bd->device, 0, sizeof(bd->device));
  bd->device.name = bd->name;
  bd->device.class = "block";
  bd->device.parent = disk ? &disk->device : 0;
  bd->device.driver_data = bd;
  bd->device.devnode = bd->name;
  bd->device.dev_major = BLOCKDEV;
  bd->device.dev_minor = dev;
  if(device_register(&bd->device) < 0){
    acquire(&blk.lock);
    bd->used = 0;
    release(&blk.lock);
    return 0;
  }
  return bd;
}

// A partition has no driver of its own: shift the sector by the partition's
// offset inside the disk and hand it to the disk's rw.
static int
part_rw(struct blkdev *bd, uint32 sector, uchar *data, uint32 count,
        int write)
{
  struct blkdev *disk = bd->disk;
  return disk->rw(disk, bd->start - disk->start + sector, data, count, write);
}

// Linux disk_name(): "mmcblk0" + 1 -> "mmcblk0p1", "sda" + 1 -> "sda1".
static int
part_name(char *buf, int n, const char *disk, int partno)
{
  int len = strlen(disk);
  if(len + 4 > n)
    return -1;
  memmove(buf, disk, len);
  char *q = buf + len;
  if(len > 0 && disk[len - 1] >= '0' && disk[len - 1] <= '9')
    *q++ = 'p';
  if(partno >= 10)
    *q++ = '0' + partno / 10;
  *q++ = '0' + partno % 10;
  *q = 0;
  return 0;
}

// Linux add_partition(): partition partno of disk, sectors start.. relative
// to the disk, becomes block device disk->dev + partno.
static struct blkdev*
add_partition(struct blkdev *disk, int partno, uint32 start, uint32 nsect)
{
  char name[16];
  if(partno < 1 || partno >= disk->minors ||
     part_name(name, sizeof(name), disk->name, partno) < 0)
    return 0;
  return blkdev_add(disk->dev + partno, name, disk->start + start, nsect,
                    disk->bsize, part_rw, 0, disk, partno, 1);
}

static uint32
le32(const uchar *p)
{
  return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32)p[3] << 24);
}

// "ssssssss-pp", lower case: the MBR disk signature and the partition number
// (Linux msdos.c set_info()).
static void
partuuid(char *buf, uint32 disksig, int partno)
{
  static char hex[] = "0123456789abcdef";
  for(int i = 0; i < 8; i++)
    buf[i] = hex[(disksig >> (28 - 4 * i)) & 0xf];
  buf[8] = '-';
  buf[9] = hex[(partno >> 4) & 0xf];
  buf[10] = hex[partno & 0xf];
  buf[11] = 0;
}

// Linux msdos_partition(): the four primary entries of an MBR.  Sector 0 is
// read through the disk's own rw, not the buffer cache, as Linux reads it
// before anyone has opened the disk.  Prints "mmcblk0: p1 p2 p3".
static int
msdos_partition(struct blkdev *disk, uchar *mbr)
{
  if(mbr[510] != 0x55 || mbr[511] != 0xaa)
    return 0;                       // no table: bare image or blank disk
  for(int i = 0; i < 4; i++){
    uchar boot = mbr[446 + i * 16];
    if(boot != 0 && boot != 0x80)
      return 0;                     // a FAT boot sector, not an MBR
  }
  uint32 disksig = le32(mbr + 440);
  printf("%s:", disk->name);
  int n = 0;
  for(int i = 0; i < 4; i++){
    const uchar *p = mbr + 446 + i * 16;
    uchar type = p[4];
    uint32 start = le32(p + 8), nsect = le32(p + 12);
    if(type == 0xee){
      printf(" <gpt: not supported>");
      break;
    }
    if(type == 0 || start == 0 || nsect == 0)
      continue;
    if(type == 0x05 || type == 0x0f || type == 0x85){
      printf(" p%d <extended: skipped>", i + 1);
      continue;
    }
    if(disk->nsect && start >= disk->nsect){
      printf(" p%d <start beyond end of disk>", i + 1);
      continue;
    }
    if(disk->nsect && nsect > disk->nsect - start){
      printf(" p%d <truncated>", i + 1);   // Linux: "beyond EOD, truncated"
      nsect = disk->nsect - start;
    }
    struct blkdev *part = add_partition(disk, i + 1, start, nsect);
    if(part == 0){
      printf(" p%d <no slot>", i + 1);
      continue;
    }
    part->ptype = type;
    partuuid(part->uuid, disksig, i + 1);
    printf(" p%d", i + 1);
    n++;
  }
  printf("\n");
  return n;
}

// Linux blk_add_partitions(): read the table and add every partition.
static int
blk_add_partitions(struct blkdev *disk)
{
  uchar *buf = (uchar *)kalloc();
  if(buf == 0)
    return -1;
  int n = -1;
  if(disk->rw(disk, 0, buf, 1, 0) < 0)
    printf("%s: unable to read partition table\n", disk->name);
  else
    n = msdos_partition(disk, buf);
  kfree((char *)buf);
  return n;
}

// Linux add_disk(): a driver hands over a whole disk.  minors reserves the
// device numbers dev .. dev+minors-1; with minors > 1 the partition table is
// scanned right away and partition N becomes device dev+N.  nsect 0 means
// the size is unknown (no bound check).
int
add_disk(int dev, const char *name, int minors, uint32 nsect, uint32 bsize,
         blk_rw_fn rw, void *priv)
{
  if(minors < 1)
    return -1;
  struct blkdev *disk = blkdev_add(dev, name, 0, nsect, bsize, rw, priv,
                                   0, 0, minors);
  if(disk == 0)
    return -1;
  if(minors > 1)
    blk_add_partitions(disk);
  return dev;
}

// Linux devt_from_partuuid(): root=PARTUUID=... names a partition on any
// disk.  Hex digits compare without case.
struct blkdev*
blkdev_find_partuuid(const char *uuid)
{
  for(int i = 0; i < NBLKDEV; i++){
    struct blkdev *bd = &blk.dev[i];
    if(!bd->used || bd->dying || bd->uuid[0] == 0)
      continue;
    int j = 0;
    for(; bd->uuid[j]; j++){
      char ch = uuid[j];
      if(ch >= 'A' && ch <= 'F')
        ch += 'a' - 'A';
      if(ch != bd->uuid[j])
        break;
    }
    if(bd->uuid[j] == 0 && uuid[j] == 0)
      return bd;
  }
  return 0;
}

// Linux del_gendisk(): remove the partitions, then the disk.  Fails, with
// whatever was already removed gone, if any of them is mounted or open.
int
del_disk(int dev)
{
  struct blkdev *disk = blkdev_get(dev);
  if(disk == 0 || disk->disk)
    return -1;
  for(int n = 1; n < disk->minors; n++){
    struct blkdev *p = blkdev_get(dev + n);
    if(p && p->disk == disk && blkdev_unregister(dev + n) < 0)
      return -1;
  }
  return blkdev_unregister(dev);
}

int
blkdev_unregister(int dev)
{
  struct blkdev *bd = 0;
  if(getsuper(dev))
    return -1;
  acquire(&blk.lock);
  for(int i = 0; i < NBLKDEV; i++)
    if(blk.dev[i].used && blk.dev[i].dev == dev){
      bd = &blk.dev[i];
      break;
    }
  if(bd == 0 || bd->dying || bd->openers){
    release(&blk.lock);
    return -1;
  }
  for(int i = 0; i < NBLKDEV; i++)
    if(blk.dev[i].used && blk.dev[i].disk == bd){
      release(&blk.lock);
      return -1;                  // partitions first (del_disk)
    }
  bd->dying = 1;
  while(bd->inflight)
    sleep(bd, &blk.lock);
  release(&blk.lock);
  if(binvalidate(dev) < 0){
    acquire(&blk.lock);
    bd->dying = 0;
    release(&blk.lock);
    return -1;
  }
  device_unregister(&bd->device);
  acquire(&blk.lock);
  bd->used = 0;
  bd->dying = 0;
  bd->rw = 0;
  bd->priv = 0;
  bd->disk = 0;
  release(&blk.lock);
  return 0;
}

struct blkdev*
blkdev_get(uint dev)
{
  for(int i = 0; i < NBLKDEV; i++)
    if(blk.dev[i].used && !blk.dev[i].dying && blk.dev[i].dev == dev)
      return &blk.dev[i];
  return 0;
}

int
blkdev_find(const char *name)
{
  for(int i = 0; i < NBLKDEV; i++)
    if(blk.dev[i].used && strncmp(blk.dev[i].name, name, 16) == 0)
      return blk.dev[i].dev;
  return -1;
}

// A filesystem chooses the block size when it mounts a device (ext2: 4096).
// Cached blocks of the old size must not exist; callers do this before the
// first bread() on the device.
int
blkdev_set_bsize(uint dev, uint32 bsize)
{
  struct blkdev *bd = blkdev_get(dev);
  if(bd == 0 || bsize < BLK_SECTOR || bsize > BLK_MAX_BSIZE ||
     bsize % BLK_SECTOR)
    return -1;
  bd->bsize = bsize;
  return 0;
}

// i-th table slot, for enumeration (devtmpfs, /proc/mounts); 0 if unused.
struct blkdev*
blkdev_at(int i)
{
  if(i < 0 || i >= NBLKDEV || !blk.dev[i].used)
    return 0;
  return &blk.dev[i];
}

uint32
blkdev_bsize(uint dev)
{
  struct blkdev *bd = blkdev_get(dev);
  return bd ? bd->bsize : 0;
}

// Move one cached block between b->data and the device.  b->lock is held, so
// no other process touches this block; sdsector() serialises the card.
int
blk_rw(struct buf *b, int write)
{
  if(!holdingsleep(&b->lock))
    panic("blk_rw: buf not locked");
  acquire(&blk.lock);
  struct blkdev *bd = 0;
  for(int i = 0; i < NBLKDEV; i++)
    if(blk.dev[i].used && !blk.dev[i].dying &&
       blk.dev[i].dev == (int)b->dev){
      bd = &blk.dev[i];
      bd->inflight++;
      break;
    }
  release(&blk.lock);
  if(bd == 0){
    printf("blk: no device %d\n", b->dev);
    return -1;
  }
  uint32 spb = bd->bsize / BLK_SECTOR;
  uint64 first = (uint64)b->blockno * spb;
  if(bd->nsect != 0 && first + spb > bd->nsect){
    printf("blk: %s block %d outside device\n", bd->name, b->blockno);
    acquire(&blk.lock);
    if(--bd->inflight == 0)
      wakeup(bd);
    release(&blk.lock);
    return -1;
  }
  if(write)                     // statistics only; a lost update is harmless
    blk.writes++;
  else
    blk.reads++;
  int result = bd->rw(bd, (uint32)first, b->data, spb, write);
  acquire(&blk.lock);
  if(--bd->inflight == 0)
    wakeup(bd);
  release(&blk.lock);
  return result;
}

void
blkdev_print(void)
{
  for(int i = 0; i < NBLKDEV; i++){
    struct blkdev *bd = &blk.dev[i];
    if(bd->used && bd->disk)
      printf("blkdev: dev %d %s (partition %d of %s) start=%d sectors=%d "
             "block=%d\n", bd->dev, bd->name, bd->partno, bd->disk->name,
             bd->start, bd->nsect, bd->bsize);
    else if(bd->used)
      printf("blkdev: dev %d %s (disk, %d minors) sectors=%d block=%d\n",
             bd->dev, bd->name, bd->minors, bd->nsect, bd->bsize);
  }
}

// Run once from the first process (bread needs a process for its sleeplock):
// read sector 0 of the whole card through the cache twice; the second read
// must be served from the cache without another device transfer.
void
blkdev_selftest(void)
{
  if(blkdev_get(BLKDEV_MMC) == 0)
    return;
  uint before = blk.reads;
  struct buf *b = bread(BLKDEV_MMC, 0);
  int err = b->error;
  int mbr = b->data[510] == 0x55 && b->data[511] == 0xaa;
  brelse(b);
  uint after_first = blk.reads;
  b = bread(BLKDEV_MMC, 0);
  err |= b->error;
  brelse(b);
  int hit = blk.reads == after_first;
  printf("blkdev: cache self-test %s (mmcblk0 sector 0: %d device read%s, "
         "%s, second read %s)\n",
         !err && hit ? "ok" : "FAILED", after_first - before,
         after_first - before == 1 ? "" : "s",
         mbr ? "MBR signature" : "no MBR", hit ? "hit" : "missed");
}

// ---------------------------------------------------------------------------
// RAM disks.  xv6's inode layer sits on the buffer cache, so the temporary
// filesystems (rootfs, devtmpfs) are ordinary xv6 filesystems on a block
// device that lives in kernel memory -- the same idea as Linux's old
// /dev/ram0 initrd.  The contents vanish at reboot.

#define RAM_MAXPAGES 32           // 128 KiB per RAM disk

static struct ramdisk {
  uint32 npages;
  char *page[RAM_MAXPAGES];
} ramdisks[2];

static int
ram_rw(struct blkdev *bd, uint32 sector, uchar *data, uint32 count, int write)
{
  struct ramdisk *rd = bd->priv;
  for(uint32 i = 0; i < count; i++){
    uint64 off = (uint64)(sector + i) * BLK_SECTOR;
    if(off / PGSIZE >= rd->npages)
      return -1;
    char *p = rd->page[off / PGSIZE] + off % PGSIZE;
    if(write)
      memmove(p, data + i * BLK_SECTOR, BLK_SECTOR);
    else
      memmove(data + i * BLK_SECTOR, p, BLK_SECTOR);
  }
  return 0;
}

// Create RAM disk n (0 or 1) as block device dev with the given size.
int
ramdisk_create(int n, int dev, char *name, uint32 bytes, uint32 bsize)
{
  if(n < 0 || n >= NELEM(ramdisks) || bytes == 0 ||
     bytes > RAM_MAXPAGES * PGSIZE)
    return -1;
  struct ramdisk *rd = &ramdisks[n];
  if(rd->npages)
    return -1;
  uint32 npages = (bytes + PGSIZE - 1) / PGSIZE;
  for(uint32 i = 0; i < npages; i++){
    if((rd->page[i] = kalloc()) == 0)
      panic("ramdisk_create");
    memset(rd->page[i], 0, PGSIZE);
  }
  rd->npages = npages;
  if(add_disk(dev, name, 1, npages * (PGSIZE / BLK_SECTOR), bsize,
              ram_rw, rd) < 0)
    panic("ramdisk_create: add_disk");
  return 0;
}

// Direct pointer to byte off of a RAM disk, for formatting before the
// buffer cache has seen the device.  A block never straddles a page.
uchar*
ramdisk_ptr(int dev, uint32 off)
{
  struct blkdev *bd = blkdev_get(dev);
  if(bd == 0 || bd->rw != ram_rw)
    return 0;
  struct ramdisk *rd = bd->priv;
  if(off / PGSIZE >= rd->npages)
    return 0;
  return (uchar *)rd->page[off / PGSIZE] + off % PGSIZE;
}

// ---------------------------------------------------------------------------
// Block device files: read(2) on /dev/mmcblk0p1 etc.  The minor number is
// the block device number.  Reads go through the buffer cache, so they are
// coherent with what the kernel itself reads through the same device number
// (/dev/root is the root partition's own device number, so it shares cache
// entries with the mounted root).

uint64
blkdev_size(uint dev)
{
  struct blkdev *bd = blkdev_get(dev);
  return bd ? (uint64)bd->nsect * BLK_SECTOR : 0;
}

static int
blkfile_open(struct file *f)
{
  acquire(&blk.lock);
  struct blkdev *bd = 0;
  for(int i = 0; i < NBLKDEV; i++)
    if(blk.dev[i].used && !blk.dev[i].dying &&
       blk.dev[i].dev == f->ip->minor){
      bd = &blk.dev[i];
      break;
    }
  if(bd)
    bd->openers++;
  release(&blk.lock);
  return bd ? 0 : -1;
}

static void
blkfile_release(struct file *f)
{
  acquire(&blk.lock);
  for(int i = 0; i < NBLKDEV; i++)
    if(blk.dev[i].used && blk.dev[i].dev == f->ip->minor){
      if(blk.dev[i].openers > 0)
        blk.dev[i].openers--;
      wakeup(&blk.dev[i]);
      break;
    }
  release(&blk.lock);
}

static int
blkfile_fread(struct file *f, int user_dst, uint64 dst, int n)
{
  struct blkdev *bd = blkdev_get(f->ip->minor);
  if(bd == 0 || n < 0)
    return -1;
  uint64 size = blkdev_size(bd->dev);     // 0: unknown, read until error
  int tot = 0;
  while(tot < n){
    if(size && f->off >= size)
      break;
    uint32 bno = f->off / bd->bsize, o = f->off % bd->bsize;
    uint32 m = bd->bsize - o;
    if(m > n - tot)
      m = n - tot;
    if(size && f->off + m > size)
      m = size - f->off;
    struct buf *b = bread(bd->dev, bno);
    if(b->error){
      brelse(b);
      break;
    }
    if(either_copyout(user_dst, dst + tot, b->data + o, m) < 0){
      brelse(b);
      return tot ? tot : -1;
    }
    brelse(b);
    f->off += m;
    tot += m;
  }
  return tot;
}

static int
blkfile_fwrite(struct file *f, int user_src, uint64 src, int n)
{
  return -1;            // block device files are read-only
}

static struct file_operations blkfile_fops = {
  .open = blkfile_open,
  .release = blkfile_release,
  .fread = blkfile_fread,
  .fwrite = blkfile_fwrite,
};
