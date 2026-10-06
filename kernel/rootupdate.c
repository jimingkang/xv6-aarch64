// Guarded in-place updater for the mounted xv6 raw-root partition.
//
// This is intentionally not a general raw-disk device.  It exposes only the
// partition selected by rootdev_init(), accepts exactly one FSSIZE image, checks
// its superblock before touching media, freezes the old root, and verifies
// every 512-byte sector after writing.  Once writing starts the old root is
// never unfrozen; the machine must be rebooted.

#include "types.h"
#include "param.h"
#include "aarch64.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "device.h"
#include "defs.h"

#define SECTOR_SIZE 512
#define HEADER_BYTES (BSIZE + SECTOR_SIZE) // boot block + superblock sector
#define PROGRESS_BYTES (4U * 1024 * 1024)

struct rootupdate_file {
  struct sleeplock lock;
  uint32 root_lba;
  uint32 root_sectors;
  uint32 accepted;
  uint32 written;
  uint32 next_progress;
  int started;
  int failed;
  uchar header[HEADER_BYTES];
  uchar sector[SECTOR_SIZE];
  uchar verify[SECTOR_SIZE];
};

static struct spinlock open_lock;
static int opened;
static int root_frozen;

int
rootupdate_root_frozen(void)
{
  int frozen;
  acquire(&open_lock);
  frozen = root_frozen;
  release(&open_lock);
  return frozen;
}

static int
valid_superblock(uchar *image)
{
  struct superblock sb;
  memmove(&sb, image + BSIZE, sizeof(sb));
  if(sb.magic != FSMAGIC || sb.size != FSSIZE ||
     sb.nblocks == 0 || sb.nblocks >= sb.size || sb.ninodes == 0 ||
     sb.nlog == 0 || sb.logstart >= sb.size ||
     sb.inodestart >= sb.size || sb.bmapstart >= sb.size)
    return -1;
  return 0;
}

static int
write_sector_verified(struct rootupdate_file *u, uint32 offset, uchar *data)
{
  uint32 sector = offset / SECTOR_SIZE;
  if(sector >= u->root_sectors ||
     sdsector(u->root_lba + sector, data, 1) < 0 ||
     sdsector(u->root_lba + sector, u->verify, 0) < 0 ||
     memcmp(data, u->verify, SECTOR_SIZE) != 0)
    return -1;
  u->written += SECTOR_SIZE;
  if(u->written >= u->next_progress){
    printf("sdroot: verified %d/%d MiB\n", u->written / (1024 * 1024),
           (FSSIZE * BSIZE) / (1024 * 1024));
    u->next_progress += PROGRESS_BYTES;
  }
  return 0;
}

static int
rootupdate_open(struct file *f)
{
  uint32 lba, sectors;
  struct rootupdate_file *u;
  if(!f->writable || f->readable ||
     rootdev_raw_info(&lba, &sectors) < 0 ||
     sectors < FSSIZE * (BSIZE / SECTOR_SIZE))
    return -1;
  acquire(&open_lock);
  if(opened){
    release(&open_lock);
    return -1;
  }
  opened = 1;
  release(&open_lock);
  if((u = kalloc()) == 0){
    acquire(&open_lock);
    opened = 0;
    release(&open_lock);
    return -1;
  }
  memset(u, 0, PGSIZE);
  initsleeplock(&u->lock, "sdroot-update");
  u->root_lba = lba;
  u->root_sectors = sectors;
  u->next_progress = PROGRESS_BYTES;
  f->private_data = u;
  printf("sdroot: armed raw root lba=%d sectors=%d; awaiting valid image\n",
         lba, sectors);
  return 0;
}

static int
rootupdate_write(struct file *f, int user_src, uint64 src, int n)
{
  struct rootupdate_file *u = f->private_data;
  uint32 image_bytes = FSSIZE * BSIZE;
  int consumed = 0;
  if(u == 0 || n <= 0 || (n % SECTOR_SIZE) != 0)
    return -1;
  acquiresleep(&u->lock);
  if(u->failed || (uint32)n > image_bytes - u->accepted)
    goto bad;

  if(!u->started){
    int need = HEADER_BYTES - u->accepted;
    int take = n < need ? n : need;
    if(either_copyin(u->header + u->accepted, user_src, src, take) < 0)
      goto bad;
    u->accepted += take;
    consumed += take;
    if(u->accepted < HEADER_BYTES){
      releasesleep(&u->lock);
      return n;
    }
    if(valid_superblock(u->header) < 0){
      printf("sdroot: rejected image: invalid xv6 superblock\n");
      goto bad;
    }
    printf("sdroot: image validated; freezing mounted root filesystem\n");
    if(rootdev_update_begin() < 0)
      goto bad;
    acquire(&open_lock);
    root_frozen = 1;
    release(&open_lock);
    u->started = 1;
    for(uint32 off = 0; off < HEADER_BYTES; off += SECTOR_SIZE)
      if(write_sector_verified(u, off, u->header + off) < 0)
        goto io_bad;
  }

  while(consumed < n){
    if(either_copyin(u->sector, user_src, src + consumed,
                     SECTOR_SIZE) < 0)
      goto io_bad;
    if(write_sector_verified(u, u->written, u->sector) < 0)
      goto io_bad;
    u->accepted += SECTOR_SIZE;
    consumed += SECTOR_SIZE;
  }
  releasesleep(&u->lock);
  return n;

io_bad:
  printf("sdroot: update failed at byte %d; root remains frozen, reboot required\n",
         u->written);
bad:
  u->failed = 1;
  releasesleep(&u->lock);
  return -1;
}

static void
rootupdate_release(struct file *f)
{
  struct rootupdate_file *u = f->private_data;
  if(u){
    if(u->started && !u->failed &&
       u->accepted == FSSIZE * BSIZE && u->written == FSSIZE * BSIZE)
      printf("sdroot: update complete and verified; reboot now\n");
    else if(u->started)
      printf("sdroot: incomplete update (%d/%d bytes); reboot/recovery required\n",
             u->written, FSSIZE * BSIZE);
    f->private_data = 0;
    kfree(u);
  }
  acquire(&open_lock);
  opened = 0;
  release(&open_lock);
}

static struct file_operations rootupdate_fops = {
  .open = rootupdate_open,
  .release = rootupdate_release,
  .fwrite = rootupdate_write,
};

void
rootupdate_init(void)
{
  initlock(&open_lock, "sdroot-open");
  opened = 0;
  root_frozen = 0;
  if(register_chrdev(ROOTUPDATE, "sdroot", &rootupdate_fops) < 0)
    panic("sdroot: register");
}
