struct buf {
  int valid;   // has data been read from disk?
  int disk;    // does disk "own" buf?
  uint dev;
  uint blockno;
  struct sleeplock lock;
  uint refcnt;
  struct buf *prev; // LRU cache list
  struct buf *next;
  int error;   // last device transfer failed (non-root devices only)
  uchar *data; // one page from kalloc(): holds a block of up to 4096 bytes;
               // the device's block size (blkdev_bsize) says how much is used
};

