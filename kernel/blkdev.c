// Block device layer: device table and block <-> sector translation.
//
// rootdev_init() registers the devices it finds while scanning the MBR:
//   dev 1  root        the xv6 root (raw partition, FS.IMG or bare image)
//   dev 2  mmcblk0     the whole SD card
//   dev 3+ mmcblk0pN   primary partition N, sector = partition start + s
// and do_mounts.c adds two RAM disks for the temporary filesystems:
//   dev 7  ram0        rootfs, the first "/" (compare Linux rootfs)
//   dev 8  ram1        devtmpfs, the kernel-maintained /dev
// Every buffer-cache miss and write-back goes through blk_rw().
//
// Block devices are also visible as device files (major BLOCKDEV, minor =
// device number): /dev/mmcblk0, /dev/mmcblk0p2, /dev/root.  They read
// through the buffer cache and are read-only from user space.

#include "types.h"
#include "aarch64.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "defs.h"
#include "blkdev.h"
#include "file.h"
#include "device.h"

static struct {
  struct spinlock lock;     // protects registration; lookups after boot are
  struct blkdev dev[NBLKDEV];  // read-only, devices are never unregistered
  uint reads, writes;       // device transfers (cache misses / write-backs)
} blk;

static struct file_operations blkfile_fops;

void
blkdev_init(void)
{
  initlock(&blk.lock, "blkdev");
  register_chrdev(BLOCKDEV, "block", &blkfile_fops);
}

// Generic rw for the whole card and its partitions.
static int
mmc_rw(struct blkdev *bd, uint32 sector, uchar *data, uint32 count, int write)
{
  for(uint32 i = 0; i < count; i++)
    if(sdsector(bd->start + sector + i, data + i * BLK_SECTOR, write) < 0)
      return -1;
  return 0;
}

int
blkdev_register(int dev, const char *name, uint32 start, uint32 nsect,
                uint32 bsize, blk_rw_fn rw)
{
  if(bsize < BLK_SECTOR || bsize > BLK_MAX_BSIZE || bsize % BLK_SECTOR)
    return -1;
  acquire(&blk.lock);
  struct blkdev *bd = 0;
  for(int i = 0; i < NBLKDEV; i++)
    if(blk.dev[i].used && blk.dev[i].dev == dev)
      bd = &blk.dev[i];            // re-registration replaces the entry
  for(int i = 0; i < NBLKDEV && bd == 0; i++)
    if(!blk.dev[i].used)
      bd = &blk.dev[i];
  if(bd == 0){
    release(&blk.lock);
    return -1;
  }
  bd->dev = dev;
  safestrcpy(bd->name, name, sizeof(bd->name));
  bd->start = start;
  bd->nsect = nsect;
  bd->bsize = bsize;
  bd->rw = rw ? rw : mmc_rw;
  bd->priv = 0;
  bd->used = 1;
  release(&blk.lock);
  return dev;
}

struct blkdev*
blkdev_get(uint dev)
{
  for(int i = 0; i < NBLKDEV; i++)
    if(blk.dev[i].used && blk.dev[i].dev == dev)
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
  struct blkdev *bd = blkdev_get(b->dev);
  if(bd == 0){
    printf("blk: no device %d\n", b->dev);
    return -1;
  }
  uint32 spb = bd->bsize / BLK_SECTOR;
  uint64 first = (uint64)b->blockno * spb;
  if(bd->nsect != 0 && first + spb > bd->nsect){
    printf("blk: %s block %d outside device\n", bd->name, b->blockno);
    return -1;
  }
  if(write)                     // statistics only; a lost update is harmless
    blk.writes++;
  else
    blk.reads++;
  return bd->rw(bd, (uint32)first, b->data, spb, write);
}

void
blkdev_print(void)
{
  for(int i = 0; i < NBLKDEV; i++){
    struct blkdev *bd = &blk.dev[i];
    if(bd->used)
      printf("blkdev: dev %d %s start=%d sectors=%d block=%d\n",
             bd->dev, bd->name, bd->start, bd->nsect, bd->bsize);
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
  if(blkdev_register(dev, name, 0, npages * (PGSIZE / BLK_SECTOR), bsize,
                     ram_rw) < 0)
    panic("ramdisk_create: register");
  blkdev_get(dev)->priv = rd;
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
// (/dev/root shares cache entries with the mounted root; /dev/mmcblk0p2 is a
// different device number and may lag behind un-synced root writes).

uint64
blkdev_size(uint dev)
{
  struct blkdev *bd = blkdev_get(dev);
  return bd ? (uint64)bd->nsect * BLK_SECTOR : 0;
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
  return -1;            // use /dev/sdroot to replace the root image
}

static struct file_operations blkfile_fops = {
  .fread = blkfile_fread,
  .fwrite = blkfile_fwrite,
};
