//
// Ramdisk containing fs.img, loaded by QEMU's generic loader.
//

#include "types.h"
#include "aarch64.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"

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
