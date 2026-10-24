// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Buffers are indexed by (dev, block)
// for any registered block device (blkdev.c); the block size is the device's,
// from 512 bytes up to one 4 KiB page, so the xv6 root (1 KiB), whole-card
// sectors (512 B) and ext2 (4 KiB) can share one cache.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include <kernel/types.h>
#include <kernel/param.h>
#include <kernel/spinlock.h>
#include <kernel/sleeplock.h>
#include "aarch64.h"
#include <kernel/defs.h>
#include <kernel/fs.h>
#include <kernel/buf.h>
#include <kernel/blkdev.h>

struct {
  struct spinlock lock;
  struct buf buf[NBUF];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  struct buf head;
} bcache;

void
binit(void)
{
  struct buf *b;

  initlock(&bcache.lock, "bcache");

  // Create linked list of buffers
  bcache.head.prev = &bcache.head;
  bcache.head.next = &bcache.head;
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    b->next = bcache.head.next;
    b->prev = &bcache.head;
    initsleeplock(&b->lock, "buffer");
    if((b->data = kalloc()) == 0)
      panic("binit: no memory for buffer data");
    bcache.head.next->prev = b;
    bcache.head.next = b;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  acquire(&bcache.lock);

  // Is the block already cached?
  for(b = bcache.head.next; b != &bcache.head; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  for(b = bcache.head.prev; b != &bcache.head; b = b->prev){
    if(b->refcnt == 0) {
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->error = 0;
      b->refcnt = 1;
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    if(blk_rw(b, 0) == 0){
      b->valid = 1;
      b->error = 0;
    } else if((int)dev == rootdev_dev()){
      panic("bread: root device read failed");
    } else {
      b->error = 1;                 // caller checks b->error; not cached
    }
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  if(blk_rw(b, 1) < 0){
    if((int)b->dev == rootdev_dev())
      panic("bwrite: root device write failed");
    b->error = 1;
  }
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  acquire(&bcache.lock);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bcache.head.next;
    b->prev = &bcache.head;
    bcache.head.next->prev = b;
    bcache.head.next = b;
  }
  
  release(&bcache.lock);
}

void
bpin(struct buf *b) {
  acquire(&bcache.lock);
  b->refcnt++;
  release(&bcache.lock);
}

void
bunpin(struct buf *b) {
  acquire(&bcache.lock);
  b->refcnt--;
  release(&bcache.lock);
}

// Drop all idle cache entries for a disappearing block device.  Refuse the
// unregister while a caller still owns a buffer, preventing a recycled
// device number from observing stale blocks from the previous device.
int
binvalidate(uint dev)
{
  acquire(&bcache.lock);
  for(struct buf *b = bcache.buf; b < bcache.buf + NBUF; b++)
    if(b->dev == dev && b->refcnt != 0){
      release(&bcache.lock);
      return -1;
    }
  for(struct buf *b = bcache.buf; b < bcache.buf + NBUF; b++)
    if(b->dev == dev){
      b->valid = 0;
      b->error = 0;
      b->dev = 0;
      b->blockno = 0;
    }
  release(&bcache.lock);
  return 0;
}
