// Root file system image linked into the kernel (.ramdisk section) for the
// ARM32 ports that have no storage driver yet; reads and writes stay in RAM.
#include "kernel/types.h"
#include "kernel/aarch64.h"
#include "kernel/param.h"
#include "kernel/spinlock.h"
#include "kernel/sleeplock.h"
#include "kernel/fs.h"
#include "kernel/buf.h"
#include "kernel/defs.h"

extern uchar ramdisk_start[], ramdisk_end[];

void
ramdisk_embed_rw(struct buf *b, int write)
{
  uint64 off = (uint64)b->blockno*BSIZE;
  if(b->dev != ROOTDEV || off+BSIZE > (uintptr)(ramdisk_end-ramdisk_start))
    panic("ramdisk bounds");
  if(write) memmove(ramdisk_start+(uintptr)off, b->data, BSIZE);
  else memmove(b->data, ramdisk_start+(uintptr)off, BSIZE);
}

// Boards without SD/FAT32 support use the RAM disk as the only root; with
// kernel/fat32.c linked in, its fat32rw() takes over and falls back here.
__attribute__((weak)) void
fat32rw(struct buf *b, int write)
{
  ramdisk_embed_rw(b, write);
}
