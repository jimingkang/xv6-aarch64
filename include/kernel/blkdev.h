#ifndef XV6_BLKDEV_H
#define XV6_BLKDEV_H

#include <kernel/device.h>

// Block device layer (compare Linux gendisk/block_device).
//
// A block device is a run of 512-byte sectors with a block size chosen by
// whoever uses it.  The buffer cache (bio.c) indexes buffers by (dev, block)
// and asks blk_rw() to move one block; blk_rw() turns the block number into
// sectors and calls the device's rw function.
//
// Registration follows Linux's three steps:
//   add_disk()           a driver registers a whole disk (gendisk) with its rw
//   blk_add_partitions() the block layer reads the partition table itself
//   add_partition()      one block device per partition, dev = disk dev + N
// A driver never registers partitions; a partition's rw forwards to its
// disk's rw with the partition start added, like /dev/mmcblk0p3 on Linux.

#define NBLKDEV       10
#define BLK_SECTOR    512
#define BLK_MAX_BSIZE 4096      // one buffer-cache page

// Fixed device numbers.  /dev/root is not a device of its own: it names one
// of these (rootdev.c chooses which, like Linux's ROOT_DEV).
#define BLKDEV_MMC        2     // whole SD card, "mmcblk0"
#define BLKDEV_MMC_MINORS 5     // mmcblk0 + p1..p4 (Linux: disk->minors)
#define BLKDEV_MMC_PART(n) (BLKDEV_MMC + (n))   // n = 1..4, "mmcblk0pN"
#define BLKDEV_RAM(n)     (7 + (n))   // n = 0..1, "ram0" rootfs, "ram1" devtmpfs

struct blkdev;
typedef int (*blk_rw_fn)(struct blkdev *bd, uint32 sector, uchar *data,
                         uint32 count, int write);

struct blkdev {
  int used;
  int dev;
  char name[16];
  uint32 start;       // first sector on the hardware; 0 for a whole disk
  uint32 nsect;       // size in sectors, 0 = unknown (no bound check)
  uint32 bsize;       // buffer-cache block size in bytes
  blk_rw_fn rw;       // sector is relative to the device
  void *priv;         // driver data (RAM disk pages)
  struct blkdev *disk;  // partitions: the whole disk; 0 for a disk
  int partno;         // 0 for a disk, N for partition N
  int minors;         // disks: device numbers reserved, dev .. dev+minors-1
  uchar ptype;        // partitions: MBR type (0x0c FAT32, 0x83 Linux, 0x7f xv6)
  char uuid[12];      // partitions: PARTUUID "ssssssss-pp" (Linux part_meta_info)
  int inflight;
  int openers;
  int dying;
  struct device device;
};

int add_disk(int dev, const char *name, int minors, uint32 nsect,
             uint32 bsize, blk_rw_fn rw, void *priv);
int del_disk(int dev);
struct blkdev *blkdev_find_partuuid(const char *uuid);
int blkdev_unregister(int dev);

#endif
