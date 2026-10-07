#ifndef XV6_BLKDEV_H
#define XV6_BLKDEV_H

// Block device layer (compare Linux gendisk/block_device).
//
// A block device is a run of 512-byte sectors with a block size chosen by
// whoever uses it.  The buffer cache (bio.c) indexes buffers by (dev, block)
// and asks blk_rw() to move one block; blk_rw() turns the block number into
// sectors and calls the device's rw function.  Partitions are just devices
// with a start offset on the whole card, like /dev/mmcblk0p3 on Linux.

#define NBLKDEV       10
#define BLK_SECTOR    512
#define BLK_MAX_BSIZE 4096      // one buffer-cache page

// Fixed device numbers.  ROOTDEV (1) is the xv6 root, wherever it lives.
#define BLKDEV_MMC        2     // whole SD card, "mmcblk0"
#define BLKDEV_MMC_PART(n) (BLKDEV_MMC + (n))   // n = 1..4, "mmcblk0pN"
#define BLKDEV_RAM(n)     (7 + (n))   // n = 0..1, "ram0" rootfs, "ram1" devtmpfs

struct blkdev;
typedef int (*blk_rw_fn)(struct blkdev *bd, uint32 sector, uchar *data,
                         uint32 count, int write);

struct blkdev {
  int used;
  int dev;
  char name[16];
  uint32 start;       // first card sector (partitions); 0 for whole card
  uint32 nsect;       // size in sectors, 0 = unknown (no bound check)
  uint32 bsize;       // buffer-cache block size in bytes
  blk_rw_fn rw;       // sector is relative to the device
  void *priv;         // driver data (RAM disk pages)
};

#endif
