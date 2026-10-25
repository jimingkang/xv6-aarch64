// xjournal: a small external sector-level transaction log.
//
// The log lives outside the filesystem it protects (for ext2 it is placed in
// the unused tail of the xv6 partition, e.g. mmcblk0p2, right after the
// FSSIZE image, and reached through that partition's block device).  A client brackets each operation with xj_begin()/xj_commit();
// xj_write() captures metadata sectors instead of writing them home, and
// xj_read() lets the client see its own uncommitted writes.  Data sectors may
// still be written directly by the client before commit (ordered mode).
//
// On-disk layout (sector offsets from the log start):
//   [0, XJ_DESC)                   descriptor: magic, seq, count, home LBAs
//   [XJ_DESC, XJ_DESC + XJ_CAP)    payload: one copy of each captured sector
//   XJ_DESC + XJ_CAP               commit record
//
// Commit protocol (every step ends with sdflush(), a CMD13 completion barrier):
//   1. descriptor + payload          4. clear the commit record
//   2. commit record  <- commit point
//   3. payload copied to home blocks through bcache
// Recovery replays a transaction only when the commit record and a checksum
// over descriptor and payload both validate.  Once a valid commit record is
// present, any unreadable or inconsistent descriptor/payload is fatal: the
// protected filesystem must not be mounted on top of an unrecovered commit.
//
// The caller serializes transactions (ext2 holds its sleeplock).

#include <kernel/types.h>
#include <kernel/param.h>
#include "aarch64.h"
#include <kernel/defs.h>
#include <kernel/blkdev.h>
#include <kernel/sleeplock.h>
#include <kernel/buf.h>

#define XJ_SECTOR 512
#define XJ_CAP    1024                       // sectors per transaction
#define XJ_DESC   9                          // ceil((16 + 4*XJ_CAP) / 512)
#define XJ_TOTAL  (XJ_DESC + XJ_CAP + 1)
#define XJ_SPP    (4096 / XJ_SECTOR)         // payload sectors per page
#define XJ_DMAGIC 0x31444a58U                // "XJD1"
#define XJ_CMAGIC 0x31434a58U                // "XJC1"

static struct {
  int ready;
  int active;
  struct blkdev *bd;   // partition holding the log (the xv6 partition)
  struct blkdev *home; // ext2 partition protected by this log
  uint32 start;        // first log sector, relative to bd
  uint32 seq;
  uint32 count;
  uint32 lba[XJ_CAP];
  char *page[XJ_CAP / XJ_SPP];
  uint32 desc[XJ_DESC * XJ_SECTOR / 4];      // descriptor sectors
  uchar commit[XJ_SECTOR];
  uchar tmp[XJ_SECTOR];
  uchar done[XJ_CAP];
} xj;

// The log itself deliberately uses a sector-level hook: its on-disk format and
// ordering barriers are expressed in 512-byte sectors.  Home sectors, however,
// are checkpointed through bread()/bwrite() below so the ext2 cache is updated
// rather than bypassed.
static int
log_rw(uint32 s, void *buf, int write)
{
  return xj.bd->rw(xj.bd, xj.start + s, buf, 1, write);
}

static uchar *
slot(uint32 i)
{
  return (uchar*)xj.page[i / XJ_SPP] + (i % XJ_SPP) * XJ_SECTOR;
}

static uint32
fnv(uint32 h, const uchar *p, uint32 n)
{
  for(uint32 i = 0; i < n; i++){
    h ^= p[i];
    h *= 16777619U;
  }
  return h;
}

static void
put32(uchar *p, uint32 v)
{
  p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

static uint32
get32(const uchar *p)
{
  return p[0] | ((uint32)p[1] << 8) | ((uint32)p[2] << 16) |
         ((uint32)p[3] << 24);
}

static uint32
txsum(uint32 count)
{
  uint32 h = fnv(2166136261U, (uchar*)xj.desc, XJ_DESC * XJ_SECTOR);
  for(uint32 i = 0; i < count; i++)
    h = fnv(h, slot(i), XJ_SECTOR);
  return h;
}

static void
make_commit(uint32 seq, uint32 count, uint32 sum)
{
  memset(xj.commit, 0, sizeof(xj.commit));
  put32(xj.commit, XJ_CMAGIC);
  put32(xj.commit + 4, seq);
  put32(xj.commit + 8, count);
  put32(xj.commit + 12, sum);
  put32(xj.commit + 16, fnv(2166136261U, xj.commit, 16));
}

static int
commit_valid(uint32 *seq, uint32 *count, uint32 *sum)
{
  if(get32(xj.commit) != XJ_CMAGIC ||
     get32(xj.commit + 16) != fnv(2166136261U, xj.commit, 16))
    return 0;
  *seq = get32(xj.commit + 4);
  *count = get32(xj.commit + 8);
  *sum = get32(xj.commit + 12);
  return *count > 0 && *count <= XJ_CAP;
}

static int
clear_commit(void)
{
  memset(xj.commit, 0, sizeof(xj.commit));
  if(log_rw(XJ_DESC + XJ_CAP, xj.commit, 1) < 0)
    return -1;
  return sdflush();
}

static int
write_home(uint32 count)
{
  if(xj.home == 0)
    return -1;
  uint32 bsize = blkdev_bsize(xj.home->dev);
  if(bsize < XJ_SECTOR || bsize > BLK_MAX_BSIZE ||
     bsize % XJ_SECTOR)
    return -1;
  uint32 spb = bsize / XJ_SECTOR;
  memset(xj.done, 0, count);
  for(uint32 i = 0; i < count; i++){
    if(xj.lba[i] < xj.home->start)
      return -1;
    uint64 rel = (uint64)xj.lba[i] - xj.home->start;
    if(xj.home->nsect && rel >= xj.home->nsect)
      return -1;
  }

  // Several logged metadata sectors commonly belong to one 4 KiB ext2
  // block.  Lock that cached block once, overlay every matching sector, and
  // issue one block write.
  for(uint32 i = 0; i < count; i++){
    if(xj.done[i])
      continue;
    uint64 rel = (uint64)xj.lba[i] - xj.home->start;
    uint32 bno = rel / spb;
    struct buf *bp = bread(xj.home->dev, bno);
    if(bp->error){
      brelse(bp);
      return -1;
    }
    for(uint32 j = i; j < count; j++){
      if(xj.done[j])
        continue;
      uint64 jrel = (uint64)xj.lba[j] - xj.home->start;
      if(jrel / spb != bno)
        continue;
      memmove(bp->data + (jrel % spb) * XJ_SECTOR,
              slot(j), XJ_SECTOR);
      xj.done[j] = 1;
    }
    bwrite(bp);
    int error = bp->error;
    brelse(bp);
    if(error)
      return -1;
  }
  return sdflush();
}

// Replay a committed but not yet checkpointed transaction.  A missing commit
// is normal.  An invalid commit sector can be retired, but after a valid
// commit record has been observed every replay error is fatal and the record
// is preserved for a later repair/retry.
static int
recover(void)
{
  uint32 seq, count, sum;
  if(log_rw(XJ_DESC + XJ_CAP, xj.commit, 0) < 0)
    return -1;
  if(get32(xj.commit) != XJ_CMAGIC)
    return 0;
  if(!commit_valid(&seq, &count, &sum)){
    printf("xjournal: invalid commit record; retiring it\n");
    return clear_commit();
  }
  for(uint32 i = 0; i < XJ_DESC; i++)
    if(log_rw(i, (uchar*)xj.desc + i * XJ_SECTOR, 0) < 0)
      return -1;
  if(xj.desc[0] != XJ_DMAGIC || xj.desc[1] != seq || xj.desc[2] != count){
    printf("xjournal: committed seq=%d has no matching descriptor\n", seq);
    return -1;
  }
  for(uint32 i = 0; i < count; i++)
    if(log_rw(XJ_DESC + i, slot(i), 0) < 0)
      return -1;
  if(txsum(count) != sum){
    printf("xjournal: committed seq=%d has a torn payload\n", seq);
    return -1;
  }
  for(uint32 i = 0; i < count; i++)
    xj.lba[i] = xj.desc[4 + i];
  if(write_home(count) < 0 || clear_commit() < 0){
    printf("xjournal: replay of seq=%d failed\n", seq);
    return -1;
  }
  xj.seq = seq;
  printf("xjournal: replayed seq=%d (%d sectors)\n", seq, count);
  return 0;
}

static void
free_pages(void)
{
  for(int i = 0; i < XJ_CAP / XJ_SPP; i++)
    if(xj.page[i]){
      kfree(xj.page[i]);
      xj.page[i] = 0;
    }
}

int
xj_init(struct blkdev *bd, uint32 start, uint32 nsectors,
        struct blkdev *home)
{
  if(xj.ready)
    return xj.bd == bd && xj.home == home && xj.start == start ? 0 : -1;
  if(nsectors < XJ_TOTAL || home == 0)
    return -1;
  for(int i = 0; i < XJ_CAP / XJ_SPP; i++){
    if((xj.page[i] = kalloc()) == 0){
      free_pages();
      return -1;
    }
  }
  xj.bd = bd;
  xj.home = home;
  xj.start = start;
  if(recover() < 0){
    printf("xjournal: recovery failed; refusing ext2 mount\n");
    free_pages();
    memset(&xj, 0, sizeof(xj));
    return -1;
  }
  xj.ready = 1;
  printf("xjournal: ready on %s+%d (lba=%d) capacity=%d sectors\n",
         bd->name, start, bd->start + start, XJ_CAP);
  return 0;
}

// Cleanly detach the singleton journal from its home filesystem.  Re-run
// recovery first because a previous checkpoint may have failed after its
// commit point; only an empty durable log can be released safely.
int
xj_shutdown(struct blkdev *home)
{
  if(!xj.ready)
    return 0;
  if(home == 0 || xj.home != home || xj.active)
    return -1;
  if(recover() < 0 || sdflush() < 0)
    return -1;
  free_pages();
  memset(&xj, 0, sizeof(xj));
  return 0;
}

int
xj_ready(void)
{
  return xj.ready;
}

uint32
xj_space(void)
{
  return xj.ready ? XJ_CAP - xj.count : 0;
}

void
xj_begin(void)
{
  if(!xj.ready)
    return;
  if(xj.active)
    panic("xj_begin: nested transaction");
  xj.active = 1;
  xj.count = 0;
}

int
xj_active(void)
{
  return xj.ready && xj.active;
}

static int
find(uint32 lba)
{
  for(uint32 i = 0; i < xj.count; i++)
    if(xj.lba[i] == lba)
      return i;
  return -1;
}

// Capture a full sector.  Returns -1 when the transaction is full; callers
// avoid that by checkpointing early (xj_space()).
int
xj_write(uint32 lba, const void *buf)
{
  if(!xj_active())
    return -1;
  int i = find(lba);
  if(i < 0){
    if(xj.count >= XJ_CAP){
      printf("xjournal: transaction overflow\n");
      return -1;
    }
    i = xj.count++;
    xj.lba[i] = lba;
  }
  memmove(slot(i), buf, XJ_SECTOR);
  return 0;
}

// Returns 1 and fills buf when lba has an uncommitted copy.
int
xj_read(uint32 lba, void *buf)
{
  if(!xj_active())
    return 0;
  int i = find(lba);
  if(i < 0)
    return 0;
  memmove(buf, slot(i), XJ_SECTOR);
  return 1;
}

int
xj_contains(uint32 lba)
{
  return xj_active() && find(lba) >= 0;
}

void
xj_abort(void)
{
  xj.count = 0;
  xj.active = 0;
}

int
xj_commit(void)
{
  uint32 count = xj.count;
  if(!xj_active())
    return 0;
  xj.active = 0;
  xj.count = 0;
  if(count == 0)
    return sdflush();          // data-only operation: still a durability point

  uint32 seq = ++xj.seq;
  memset(xj.desc, 0, XJ_DESC * XJ_SECTOR);
  xj.desc[0] = XJ_DMAGIC;
  xj.desc[1] = seq;
  xj.desc[2] = count;
  for(uint32 i = 0; i < count; i++)
    xj.desc[4 + i] = xj.lba[i];
  uint32 sum = txsum(count);

  // 1. descriptor and payload; this flush also orders earlier data writes.
  for(uint32 i = 0; i < XJ_DESC; i++)
    if(log_rw(i, (uchar*)xj.desc + i * XJ_SECTOR, 1) < 0)
      goto fail;
  for(uint32 i = 0; i < count; i++)
    if(log_rw(XJ_DESC + i, slot(i), 1) < 0)
      goto fail;
  if(sdflush() < 0)
    goto fail;
  // 2. commit record: the transaction is durable once this sector is.
  make_commit(seq, count, sum);
  if(log_rw(XJ_DESC + XJ_CAP, xj.commit, 1) < 0 || sdflush() < 0)
    goto fail;
  // 3./4. checkpoint and retire.  A failure here is repaired at next boot.
  if(write_home(count) < 0 || clear_commit() < 0)
    printf("xjournal: checkpoint of seq=%d incomplete; replay at boot\n", seq);
  return 0;

fail:
  printf("xjournal: commit of seq=%d failed before commit point\n", seq);
  return -1;
}
