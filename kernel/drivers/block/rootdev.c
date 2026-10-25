// Choosing the root block device (compare Linux init/do_mounts.c:
// prepare_namespace() and name_to_dev_t()).
//
// rootdev_init() is not a driver and does no I/O of its own beyond probing.
// By the time it runs the SD driver (sdhost.c) has added mmcblk0 with
// add_disk() and the block layer has added its partitions (blkdev.c).  This
// file only decides which of those block devices is the root, ROOT_DEV in
// Linux terms, and do_mounts.c creates /dev/root with that number and mounts
// it.  There is no wrapper device: /dev/root is the partition itself.
//
//   xv6fs root  a partition (type 0x7f preferred) whose block 1 holds the
//               xv6 superblock, e.g. mmcblk0p2 = dev 4
//   ext2 root   the partition named by root=, e.g. mmcblk0p3 = dev 5
//   bare        QEMU attaches fs.img itself as the whole disk: mmcblk0
//
// Which partition is the root follows the embedded-Linux rule: the kernel
// command line's root= (cmdline.c) names it -- /dev/mmcblk0p2 or
// PARTUUID=<MBR disk signature>-02 -- and the name is resolved against the
// block devices that exist.  Without root=, or if the named partition holds
// no xv6 filesystem, auto-detection runs: type 0x7f first, then any
// partition whose block 1 carries the xv6 superblock magic.  The xv6
// partition is looked for even with an ext2 root, because the ext2 journal
// lives in its unused tail (ext2.c).

#include <kernel/types.h>
#include "aarch64.h"
#include <kernel/param.h>
#include <kernel/spinlock.h>
#include <kernel/sleeplock.h>
#include <kernel/fs.h>
#include <kernel/buf.h>
#include <kernel/defs.h>
#include <kernel/fat32.h>
#include <kernel/blkdev.h>
#include <kernel/fs_trace.h>
#include <kernel/module.h>

#define SECTOR_SIZE        512
#define SECTORS_PER_BLOCK  (BSIZE / SECTOR_SIZE)
#define XV6_PARTITION_TYPE 0x7f

static uchar sector[SECTOR_SIZE];

// ROOT_DEV: the block device /dev/root names; -1 until rootdev_init().
static int root_dev = -1;
// The partition (or bare disk) holding an xv6 image, whatever the root is.
static struct blkdev *xv6_part;

// rootfstype= : xv6fs (default) mounts xv6_part; ext2 mounts partition
// ext2_root_part (see do_mounts.c).
static int root_fstype = ROOTFS_XV6;
static int ext2_root_part;

static uint32
le32(const uchar *p)
{
  return (uint32)p[0] | ((uint32)p[1] << 8) |
         ((uint32)p[2] << 16) | ((uint32)p[3] << 24);
}

static char *
partition_type_name(uchar type)
{
  switch(type){
  case 0x00: return "empty";
  case 0x01: return "FAT12";
  case 0x04: return "FAT16<32M";
  case 0x06: return "FAT16";
  case 0x0b: return "FAT32";
  case 0x0c: return "FAT32-LBA";
  case 0x0e: return "FAT16-LBA";
  case 0x83: return "Linux";
  case XV6_PARTITION_TYPE: return "xv6 raw filesystem";
  case 0xee: return "GPT-protective";
  default:   return "unknown";
  }
}

// Read one sector of a block device through its own rw.  The buffer cache
// is not used: no process exists yet to sleep on a buffer lock.
static int
bdev_read(struct blkdev *bd, uint32 s, uchar *buf)
{
  if(bd->nsect != 0 && s >= bd->nsect)
    return -1;
  return bd->rw(bd, s, buf, 1, 0);
}

// Partition n of disk, or 0.
static struct blkdev*
disk_part(struct blkdev *disk, int n)
{
  struct blkdev *p = blkdev_get(disk->dev + n);
  return p && p->disk == disk ? p : 0;
}

// Does this block device start with an xv6 image?  Block 1 is the
// superblock; with 1024-byte blocks it begins two sectors in.
static int
is_xv6fs(struct blkdev *bd)
{
  if((bd->nsect != 0 && bd->nsect < FSSIZE * SECTORS_PER_BLOCK) ||
     bdev_read(bd, SECTORS_PER_BLOCK, sector) < 0 ||
     le32(sector) != FSMAGIC || le32(sector + 4) != FSSIZE)
    return 0;
  FSTRACE("root probe: xv6fs on %s magic=%x blocks=%d\n",
          bd->name, le32(sector), le32(sector + 4));
  return 1;
}

// name_to_dev_t() for the names xv6 knows: /dev/<name>, <name> or
// PARTUUID=ssssssss-pp.  Returns the block device or 0.
static struct blkdev*
name_to_bdev(char *spec)
{
  if(strncmp(spec, "PARTUUID=", 9) == 0)
    return blkdev_find_partuuid(spec + 9);
  if(strncmp(spec, "/dev/", 5) == 0)
    spec += 5;
  int dev = blkdev_find(spec);
  return dev < 0 ? 0 : blkdev_get(dev);
}

// rootfstype=xv6fs (default) or ext2.  Anything else is reported and the
// xv6fs default is kept instead of failing the boot.
static void
check_rootfstype(void)
{
  char t[16];
  if(!cmdline_get("rootfstype", t, sizeof(t)) || strncmp(t, "xv6fs", 6) == 0 ||
     strncmp(t, "xv6", 4) == 0)
    return;
  if(strncmp(t, "ext2", 5) == 0){
    root_fstype = ROOTFS_EXT2;
    return;
  }
  printf("rootdev: rootfstype=%s not supported, using xv6fs\n", t);
}

int
rootdev_fstype(void)
{
  return root_fstype;
}

// MBR partition (1..4) holding the ext2 root, or 0.
int
rootdev_ext2_part(void)
{
  return root_fstype == ROOTFS_EXT2 ? ext2_root_part : 0;
}

// ROOT_DEV, the device number behind /dev/root; -1 before rootdev_init().
int
rootdev_dev(void)
{
  return root_dev;
}

// The block device holding the xv6 image, or 0 (ext2 keeps its journal in
// the tail of it).
struct blkdev*
rootdev_xv6_part(void)
{
  return xv6_part;
}

void
rootdev_init(void)
{
  // The boot disk.  Linux waits for whatever root= names; xv6 boots from
  // the SD card, so its driver must have added mmcblk0 by now.
  struct blkdev *disk = blkdev_get(BLKDEV_MMC);
  if(disk == 0)
    panic("rootdev: no mmcblk0 (SD driver probe failed?)");

  // Partitions come from the block layer's MBR scan, not from here.
  int nparts = 0;
  for(int n = 1; n < disk->minors; n++){
    struct blkdev *p = disk_part(disk, n);
    if(p == 0)
      continue;
    if(nparts++ == 0)
      printf("rootdev: partitions of %s\n", disk->name);
    printf("rootdev:   %s type=0x%x (%s) start=%d sectors=%d size=%d MiB "
           "partuuid=%s\n", p->name, p->ptype, partition_type_name(p->ptype),
           p->start, p->nsect, p->nsect / 2048, p->uuid);
  }

  if(nparts == 0){
    // QEMU development mode attaches fs.img itself as the SD card.
    cmdline_init();
    if(!is_xv6fs(disk))
      panic("rootdev: no partitions and no xv6 image on mmcblk0");
    xv6_part = disk;
    root_dev = disk->dev;
    printf("rootdev: no partitions; root is the bare xv6 image on %s\n",
           disk->name);
    return;
  }

  // bootfs is needed for /boot, BCM/MT7601 firmware and cmdline_xv6.txt.
  for(int n = 1; n < disk->minors; n++){
    struct blkdev *p = disk_part(disk, n);
    if(p && (p->ptype == 0x0b || p->ptype == 0x0c) &&
       fat32mount(p->start, p->nsect) == 0){
      printf("rootdev: bootfs %s lba=%d mounted as FAT32\n", p->name, p->start);
      break;
    }
  }

  cmdline_init();
  check_rootfstype();

  // root= from the command line, resolved against the block devices.
  char spec[64];
  struct blkdev *named = 0;
  int have_root = cmdline_get("root", spec, sizeof(spec)) && spec[0];
  if(have_root)
    named = name_to_bdev(spec);
  if(have_root && root_fstype == ROOTFS_EXT2){
    if(named && named->disk == disk){
      ext2_root_part = named->partno;
      root_dev = named->dev;
      printf("rootdev: root=%s rootfstype=ext2 -> %s\n", spec, named->name);
    } else {
      printf("rootdev: root=%s: no such partition, using xv6fs\n", spec);
      root_fstype = ROOTFS_XV6;
    }
  } else if(have_root){
    if(named && named->disk && is_xv6fs(named)){
      xv6_part = named;
      printf("rootdev: root=%s -> %s\n", spec, named->name);
    } else if(named == 0){
      printf("rootdev: root=%s: unknown device name, auto-detecting\n",
             spec);
    } else {
      printf("rootdev: root=%s: no xv6 filesystem there, auto-detecting\n",
             spec);
    }
  }

  // Auto-detection.  Prefer type 0x7f, but accept any other partition whose
  // block-1 superblock has the xv6 magic: macOS diskutil can create Linux
  // partitions but not an arbitrary type.  FAT partitions are skipped.
  for(int pass = 0; pass < 2 && xv6_part == 0; pass++){
    for(int n = 1; n < disk->minors; n++){
      struct blkdev *p = disk_part(disk, n);
      if(p == 0 || (pass == 0) != (p->ptype == XV6_PARTITION_TYPE))
        continue;
      if(p->ptype != 0x0b && p->ptype != 0x0c && is_xv6fs(p)){
        xv6_part = p;
        break;
      }
    }
  }
  if(xv6_part)
    printf("rootdev: xv6 filesystem on %s start=%d sectors=%d size=%d MiB\n",
           xv6_part->name, xv6_part->start, xv6_part->nsect,
           xv6_part->nsect / 2048);

  if(root_fstype == ROOTFS_EXT2){
    if(xv6_part == 0)
      printf("rootdev: no xv6 partition; ext2 root without its journal\n");
  } else {
    if(xv6_part == 0)
      panic("rootdev: no xv6 partition for an xv6fs root");
    root_dev = xv6_part->dev;
  }
  printf("rootdev: ROOT_DEV = %d (%s)\n", root_dev, blkdev_get(root_dev)->name);
}

// Level late: mounting the root is what Linux does after do_initcalls(),
// by which time every block driver above has had its probe run.
late_initcall("rootdev", rootdev_init);
