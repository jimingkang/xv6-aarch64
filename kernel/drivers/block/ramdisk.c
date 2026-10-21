//
// Ramdisk containing fs.img, loaded by QEMU's generic loader.
//

#include <kernel/types.h>
#include "aarch64.h"
#include <kernel/defs.h>
#include <kernel/param.h>
#include "memlayout.h"
#include <kernel/spinlock.h>
#include <kernel/sleeplock.h>
#include <kernel/fs.h>
#include <kernel/buf.h>

void
ramdiskinit(void)
{
}

void
ramdiskrw(struct buf *b, int write)
{
  if(!holdingsleep(&b->lock))
    panic("ramdiskrw: buf not locked");

  if(b->blockno >= FSSIZE)
    panic("ramdiskrw: blockno too big");

  uint64 diskaddr = b->blockno * BSIZE;
  char *addr = (char *)RAMDISK + diskaddr;

  if(write){
    memmove(addr, b->data, BSIZE);
  } else {
    memmove(b->data, addr, BSIZE);
  }
}
