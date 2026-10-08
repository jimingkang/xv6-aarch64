// Small ext2 filesystem backend for the VFS mount at /mnt/ext2.
//
// It supports classic ext2 regular files, linear directories and all three
// levels of indirect blocks.  Operations are serialized by the e2.lock
// sleeplock (SD transfers are slow polled I/O; a spinlock would keep
// interrupts off on this CPU for the whole operation).
//
// ext2 itself has no journal.  When a raw xv6 root partition exists, every
// mutating operation becomes one transaction in an external journal kept in
// p2 after the xv6 image (kernel/xjournal.c): metadata is captured and
// committed atomically, file data is written in place before the commit.

#include "types.h"
#include "aarch64.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "ext2.h"
#include "fat32.h"

#define SECTOR 512
#define EXT2_MAGIC 0xef53
#define EXT2_ROOT_INO 2
#define EXT2_S_IFMT  0xf000
#define EXT2_S_IFDIR 0x4000
#define EXT2_S_IFREG 0x8000

struct einode {
  uint32 ino;
  uint16 mode;
  uint16 links;
  uint64 size;
  uint32 sectors;
  uint32 flags;
  uint32 block[15];
};

#define E2_MAXFREED 256

static struct {
  struct sleeplock lock;
  int ready;
  int dirty;
  int journal;          // external xjournal transactions enabled
  int direct;           // current write bypasses the journal (file data)
  uint32 nfreed;        // blocks freed in the current transaction
  uint32 freed[E2_MAXFREED];
  struct { uint32 ino; int ref; int orphan; } open[NFILE];
  uint32 part_lba;
  uint32 part_sectors;
  uint32 blocks;
  uint32 inodes;
  uint32 first_data_block;
  uint32 first_ino;
  uint32 block_size;
  uint32 blocks_per_group;
  uint32 inodes_per_group;
  uint32 inode_size;
  uint32 desc_size;
  uint32 groups;
  uint32 gd_block;
} e2;

static uchar sectorbuf[SECTOR];
static uchar blockbuf[4096];
static uchar inodebuf[512];
// Pointer blocks are shared scratch storage protected by e2.lock.  Three
// independent pages are needed while walking/pruning a triple-indirect path.
static uint32 ptrbuf[3][1024];

static uint16 le16(const uchar *p) { return p[0] | ((uint16)p[1] << 8); }
static uint32 le32(const uchar *p) {
  return p[0] | ((uint32)p[1] << 8) | ((uint32)p[2] << 16) |
         ((uint32)p[3] << 24);
}
static void put16(uchar *p, uint16 v) { p[0] = v; p[1] = v >> 8; }
static void put32(uchar *p, uint32 v) {
  p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

// All ext2 sector I/O goes through these two functions so the journal can
// capture metadata writes and serve them back to later reads.
static int
e2_rsector(uint32 lba, void *buf)
{
  if(e2.journal && xj_read(lba, buf))
    return 0;
  return sdsector(lba, buf, 0);
}

static int
e2_wsector(uint32 lba, const void *buf)
{
  if(e2.journal && xj_active() && (!e2.direct || xj_contains(lba)))
    return xj_write(lba, buf);
  return sdsector(lba, (void*)buf, 1);
}

static int
diskbytes(uint64 byte, void *dst, uint32 n)
{
  uchar *p = dst;
  if(byte + n < byte || byte + n > (uint64)e2.part_sectors * SECTOR)
    return -1;
  while(n){
    uint32 lba = e2.part_lba + byte / SECTOR;
    uint32 off = byte % SECTOR;
    uint32 take = SECTOR - off;
    if(take > n) take = n;
    if(e2_rsector(lba, sectorbuf) < 0) return -1;
    memmove(p, sectorbuf + off, take);
    p += take; byte += take; n -= take;
  }
  return 0;
}

static int
diskbytes_write(uint64 byte, const void *src, uint32 n)
{
  const uchar *p = src;
  if(byte + n < byte || byte + n > (uint64)e2.part_sectors * SECTOR)
    return -1;
  while(n){
    uint32 lba = e2.part_lba + byte / SECTOR;
    uint32 off = byte % SECTOR;
    uint32 take = SECTOR - off;
    if(take > n) take = n;
    if(off == 0 && take == SECTOR){
      if(e2_wsector(lba, p) < 0) return -1;
    } else {
      if(e2_rsector(lba, sectorbuf) < 0) return -1;
      memmove(sectorbuf + off, p, take);
      if(e2_wsector(lba, sectorbuf) < 0) return -1;
    }
    p += take; byte += take; n -= take;
  }
  return 0;
}

static int
readblock(uint32 bno, void *dst)
{
  if(bno == 0 || bno >= e2.blocks) return -1;
  return diskbytes((uint64)bno * e2.block_size, dst, e2.block_size);
}

static int
writeblock(uint32 bno, const void *src)
{
  if(bno == 0 || bno >= e2.blocks) return -1;
  return diskbytes_write((uint64)bno * e2.block_size, src, e2.block_size);
}

// File contents bypass the journal (ordered mode): they reach the card before
// the transaction that makes them reachable commits.
static int
writeblock_data(uint32 bno, const void *src)
{
  e2.direct = 1;
  int r = writeblock(bno, src);
  e2.direct = 0;
  return r;
}

static int
readgd(uint32 group, uchar *gd)
{
  if(group >= e2.groups || e2.desc_size > sizeof(inodebuf)) return -1;
  return diskbytes((uint64)e2.gd_block * e2.block_size +
                   group * e2.desc_size, gd, e2.desc_size);
}

static int
writegd(uint32 group, uchar *gd)
{
  if(group >= e2.groups || e2.desc_size > sizeof(inodebuf)) return -1;
  return diskbytes_write((uint64)e2.gd_block * e2.block_size +
                         group * e2.desc_size, gd, e2.desc_size);
}

static int
readinode(uint32 ino, struct einode *ip)
{
  uint32 group, index, table;
  uint64 gd_off, off;
  if(!e2.ready || ino == 0 || ino > e2.inodes) return -1;
  group = (ino - 1) / e2.inodes_per_group;
  index = (ino - 1) % e2.inodes_per_group;
  if(group >= e2.groups || e2.inode_size > sizeof(inodebuf)) return -1;
  gd_off = (uint64)e2.gd_block * e2.block_size + group * e2.desc_size;
  if(diskbytes(gd_off, inodebuf, e2.desc_size) < 0) return -1;
  table = le32(inodebuf + 8);
  off = (uint64)table * e2.block_size + (uint64)index * e2.inode_size;
  if(diskbytes(off, inodebuf, e2.inode_size) < 0) return -1;
  ip->ino = ino;
  ip->mode = le16(inodebuf);
  ip->links = le16(inodebuf + 26);
  ip->size = le32(inodebuf + 4);
  if((ip->mode & EXT2_S_IFMT) == EXT2_S_IFREG)
    ip->size |= (uint64)le32(inodebuf + 108) << 32;
  ip->sectors = le32(inodebuf + 28);
  ip->flags = le32(inodebuf + 32);
  for(int i = 0; i < 15; i++) ip->block[i] = le32(inodebuf + 40 + i*4);
  return 0;
}

static int
writeinode(struct einode *ip)
{
  uint32 group, index, table;
  uint64 off;
  if(ip->ino == 0 || ip->ino > e2.inodes) return -1;
  group = (ip->ino - 1) / e2.inodes_per_group;
  index = (ip->ino - 1) % e2.inodes_per_group;
  if(readgd(group, inodebuf) < 0) return -1;
  table = le32(inodebuf + 8);
  off = (uint64)table * e2.block_size + (uint64)index * e2.inode_size;
  if(diskbytes(off, inodebuf, e2.inode_size) < 0) return -1;
  put16(inodebuf, ip->mode);
  put32(inodebuf + 4, (uint32)ip->size);
  put16(inodebuf + 26, ip->links);
  put32(inodebuf + 28, ip->sectors);
  put32(inodebuf + 32, ip->flags);
  for(int i = 0; i < 15; i++) put32(inodebuf + 40 + i*4, ip->block[i]);
  if(e2.inode_size >= 112 && (ip->mode & EXT2_S_IFMT) == EXT2_S_IFREG)
    put32(inodebuf + 108, ip->size >> 32);
  return diskbytes_write(off, inodebuf, e2.inode_size);
}

static int
clearinode(uint32 ino)
{
  uchar gd[64];
  uint32 group = (ino - 1) / e2.inodes_per_group;
  uint32 index = (ino - 1) % e2.inodes_per_group;
  if(ino == 0 || ino > e2.inodes || readgd(group, gd) < 0) return -1;
  uint32 table = le32(gd + 8);
  uint64 off = (uint64)table * e2.block_size + (uint64)index * e2.inode_size;
  memset(inodebuf, 0, e2.inode_size);
  return diskbytes_write(off, inodebuf, e2.inode_size);
}

static uint64
max_file_blocks(void)
{
  uint64 n = e2.block_size / 4;
  return 12 + n + n*n + n*n*n;
}

static uint32
fileblock(struct einode *ip, uint64 logical)
{
  uint64 n = e2.block_size / 4;
  if(logical < 12) return ip->block[logical];
  logical -= 12;
  if(logical < n){
    if(ip->block[12] == 0 || readblock(ip->block[12], ptrbuf[0]) < 0)
      return 0;
    return ptrbuf[0][logical];
  }
  logical -= n;
  if(logical < n*n){
    if(ip->block[13] == 0 || readblock(ip->block[13], ptrbuf[0]) < 0)
      return 0;
    uint32 child = ptrbuf[0][logical / n];
    if(child == 0 || readblock(child, ptrbuf[1]) < 0)
      return 0;
    return ptrbuf[1][logical % n];
  }
  logical -= n*n;
  if(logical >= n*n*n || ip->block[14] == 0 ||
     readblock(ip->block[14], ptrbuf[0]) < 0)
    return 0;
  uint64 per_top = n*n;
  uint32 middle = ptrbuf[0][logical / per_top];
  if(middle == 0 || readblock(middle, ptrbuf[1]) < 0)
    return 0;
  logical %= per_top;
  uint32 leaf = ptrbuf[1][logical / n];
  if(leaf == 0 || readblock(leaf, ptrbuf[2]) < 0)
    return 0;
  return ptrbuf[2][logical % n];
}

static int
adjust_super_count(uint32 offset, int delta)
{
  uchar v[4];
  if(diskbytes(1024 + offset, v, sizeof(v)) < 0) return -1;
  put32(v, le32(v) + delta);
  return diskbytes_write(1024 + offset, v, sizeof(v));
}

static int
mark_write_session_dirty(void)
{
  uchar state[2];
  if(e2.dirty) return 0;
  if(diskbytes(1024 + 58, state, sizeof(state)) < 0) return -1;
  put16(state, 0); // clear EXT2_VALID_FS until a future clean-unmount exists
  e2.direct = 1;   // outside any transaction: stays set even if one aborts
  int r = diskbytes_write(1024 + 58, state, sizeof(state));
  e2.direct = 0;
  if(r < 0 || sdflush() < 0) return -1;
  e2.dirty = 1;
  printf("ext2: write session active; run e2fsck after unclean power loss\n");
  return 0;
}

// A block freed earlier in the running transaction must not be reused (and
// overwritten in place) before the free commits: an abort or a crash would
// hand it back to its old owner with foreign contents.
static int
freed_in_tx(uint32 bno)
{
  for(uint32 i = 0; i < e2.nfreed; i++)
    if(e2.freed[i] == bno) return 1;
  return 0;
}

static int
alloc_block(uint32 preferred_group, uint32 *out)
{
  uchar gd[64];
  for(uint32 pass = 0; pass < e2.groups; pass++){
    uint32 group = (preferred_group + pass) % e2.groups;
    uint32 first = e2.first_data_block + group * e2.blocks_per_group;
    uint32 count = e2.blocks_per_group;
    if(first >= e2.blocks) continue;
    if(count > e2.blocks - first) count = e2.blocks - first;
    if(readgd(group, gd) < 0 || le16(gd + 12) == 0) continue;
    uint32 bitmap = le32(gd);
    if(readblock(bitmap, blockbuf) < 0) return -1;
    for(uint32 bit = 0; bit < count; bit++){
      if((blockbuf[bit >> 3] & (1U << (bit & 7))) != 0) continue;
      if(freed_in_tx(first + bit)) continue;   // still owned if we abort
      blockbuf[bit >> 3] |= 1U << (bit & 7);
      if(writeblock(bitmap, blockbuf) < 0) return -1;
      put16(gd + 12, le16(gd + 12) - 1);
      if(writegd(group, gd) < 0 || adjust_super_count(12, -1) < 0)
        return -1;
      *out = first + bit;
      // The block is unreachable until this transaction commits, so it can
      // be zeroed in place instead of through the journal.
      memset(blockbuf, 0, e2.block_size);
      if(writeblock_data(*out, blockbuf) < 0) return -1;
      return 0;
    }
  }
  return -1;
}

static int
free_block(uint32 bno)
{
  uchar gd[64];
  if(bno < e2.first_data_block || bno >= e2.blocks) return -1;
  uint32 group = (bno - e2.first_data_block) / e2.blocks_per_group;
  uint32 bit = (bno - e2.first_data_block) % e2.blocks_per_group;
  if(readgd(group, gd) < 0) return -1;
  uint32 bitmap = le32(gd);
  if(readblock(bitmap, blockbuf) < 0 ||
     (blockbuf[bit >> 3] & (1U << (bit & 7))) == 0) return -1;
  blockbuf[bit >> 3] &= ~(1U << (bit & 7));
  if(writeblock(bitmap, blockbuf) < 0) return -1;
  put16(gd + 12, le16(gd + 12) + 1);
  if(e2.nfreed < E2_MAXFREED) e2.freed[e2.nfreed++] = bno;
  return writegd(group, gd) < 0 ? -1 : adjust_super_count(12, 1);
}

static int
alloc_inode(uint32 preferred_group, uint16 mode, struct einode *ip)
{
  uchar gd[64];
  for(uint32 pass = 0; pass < e2.groups; pass++){
    uint32 group = (preferred_group + pass) % e2.groups;
    uint32 count = e2.inodes_per_group;
    uint32 first = group * e2.inodes_per_group + 1;
    if(first > e2.inodes) continue;
    if(count > e2.inodes - first + 1) count = e2.inodes - first + 1;
    if(readgd(group, gd) < 0 || le16(gd + 14) == 0) continue;
    uint32 bitmap = le32(gd + 4);
    if(readblock(bitmap, blockbuf) < 0) return -1;
    for(uint32 bit = 0; bit < count; bit++){
      uint32 ino = first + bit;
      if(ino < e2.first_ino ||
         (blockbuf[bit >> 3] & (1U << (bit & 7))) != 0) continue;
      blockbuf[bit >> 3] |= 1U << (bit & 7);
      if(writeblock(bitmap, blockbuf) < 0) return -1;
      put16(gd + 14, le16(gd + 14) - 1);
      if((mode & EXT2_S_IFMT) == EXT2_S_IFDIR)
        put16(gd + 16, le16(gd + 16) + 1);
      if(writegd(group, gd) < 0 || adjust_super_count(16, -1) < 0)
        return -1;
      if(clearinode(ino) < 0) return -1;
      memset(ip, 0, sizeof(*ip));
      ip->ino = ino;
      ip->mode = mode;
      ip->links = 1;
      if(writeinode(ip) < 0) return -1;
      return 0;
    }
  }
  return -1;
}

static int
free_inode(uint32 ino, int directory)
{
  uchar gd[64];
  if(ino < e2.first_ino || ino > e2.inodes) return -1;
  uint32 group = (ino - 1) / e2.inodes_per_group;
  uint32 bit = (ino - 1) % e2.inodes_per_group;
  if(readgd(group, gd) < 0) return -1;
  uint32 bitmap = le32(gd + 4);
  if(readblock(bitmap, blockbuf) < 0 ||
     (blockbuf[bit >> 3] & (1U << (bit & 7))) == 0) return -1;
  blockbuf[bit >> 3] &= ~(1U << (bit & 7));
  if(writeblock(bitmap, blockbuf) < 0) return -1;
  put16(gd + 14, le16(gd + 14) + 1);
  if(directory){
    uint16 used = le16(gd + 16);
    if(used == 0) return -1;
    put16(gd + 16, used - 1);
  }
  if(writegd(group, gd) < 0 || adjust_super_count(16, 1) < 0)
    return -1;
  return clearinode(ino);
}

static int
alloc_pointer_block(struct einode *ip, uint32 group, uint32 *slot)
{
  if(alloc_block(group, slot) < 0)
    return -1;
  ip->sectors += e2.block_size / SECTOR;
  return 0;
}

static int
set_fileblock(struct einode *ip, uint64 logical, uint32 bno)
{
  uint64 n = e2.block_size / 4;
  uint32 group = (ip->ino - 1) / e2.inodes_per_group;
  if(logical < 12){
    ip->block[logical] = bno;
    return 0;
  }
  logical -= 12;
  if(logical < n){
    if(ip->block[12] == 0){
      if(bno == 0) return 0;
      if(alloc_pointer_block(ip, group, &ip->block[12]) < 0) return -1;
    }
    if(readblock(ip->block[12], ptrbuf[0]) < 0) return -1;
    ptrbuf[0][logical] = bno;
    return writeblock(ip->block[12], ptrbuf[0]);
  }
  logical -= n;
  if(logical < n*n){
    uint32 top = logical / n, leaf = logical % n;
    if(ip->block[13] == 0){
      if(bno == 0) return 0;
      if(alloc_pointer_block(ip, group, &ip->block[13]) < 0) return -1;
    }
    if(readblock(ip->block[13], ptrbuf[0]) < 0) return -1;
    if(ptrbuf[0][top] == 0){
      if(bno == 0) return 0;
      if(alloc_pointer_block(ip, group, &ptrbuf[0][top]) < 0 ||
         writeblock(ip->block[13], ptrbuf[0]) < 0) return -1;
    }
    if(readblock(ptrbuf[0][top], ptrbuf[1]) < 0) return -1;
    ptrbuf[1][leaf] = bno;
    return writeblock(ptrbuf[0][top], ptrbuf[1]);
  }
  logical -= n*n;
  if(logical >= n*n*n) return -1;
  uint32 top = logical / (n*n);
  uint32 middle = (logical / n) % n;
  uint32 leaf = logical % n;
  if(ip->block[14] == 0){
    if(bno == 0) return 0;
    if(alloc_pointer_block(ip, group, &ip->block[14]) < 0) return -1;
  }
  if(readblock(ip->block[14], ptrbuf[0]) < 0) return -1;
  if(ptrbuf[0][top] == 0){
    if(bno == 0) return 0;
    if(alloc_pointer_block(ip, group, &ptrbuf[0][top]) < 0 ||
       writeblock(ip->block[14], ptrbuf[0]) < 0) return -1;
  }
  if(readblock(ptrbuf[0][top], ptrbuf[1]) < 0) return -1;
  if(ptrbuf[1][middle] == 0){
    if(bno == 0) return 0;
    if(alloc_pointer_block(ip, group, &ptrbuf[1][middle]) < 0 ||
       writeblock(ptrbuf[0][top], ptrbuf[1]) < 0) return -1;
  }
  if(readblock(ptrbuf[1][middle], ptrbuf[2]) < 0) return -1;
  ptrbuf[2][leaf] = bno;
  return writeblock(ptrbuf[1][middle], ptrbuf[2]);
}

static uint32
get_or_alloc_fileblock(struct einode *ip, uint64 logical)
{
  uint32 bno = fileblock(ip, logical);
  if(bno) return bno;
  uint32 group = (ip->ino - 1) / e2.inodes_per_group;
  if(alloc_block(group, &bno) < 0 || set_fileblock(ip, logical, bno) < 0)
    return 0;
  ip->sectors += e2.block_size / SECTOR;
  return bno;
}

static int
ptr_empty(uint32 *p)
{
  for(uint32 i = 0; i < e2.block_size / 4; i++)
    if(p[i]) return 0;
  return 1;
}

static void
account_free(struct einode *ip)
{
  uint32 sectors = e2.block_size / SECTOR;
  ip->sectors = ip->sectors >= sectors ? ip->sectors - sectors : 0;
}

// Free one data block and prune now-empty indirect blocks on its path.
static int
clear_fileblock(struct einode *ip, uint64 logical)
{
  uint64 n = e2.block_size / 4;
  if(logical < 12){
    if(ip->block[logical] && free_block(ip->block[logical]) < 0) return -1;
    if(ip->block[logical]) account_free(ip);
    ip->block[logical] = 0;
    return 0;
  }
  logical -= 12;
  if(logical < n){
    uint32 root = ip->block[12];
    if(root == 0) return 0;
    if(readblock(root, ptrbuf[0]) < 0) return -1;
    if(ptrbuf[0][logical]){
      if(free_block(ptrbuf[0][logical]) < 0) return -1;
      account_free(ip); ptrbuf[0][logical] = 0;
    }
    if(ptr_empty(ptrbuf[0])){
      if(free_block(root) < 0) return -1;
      account_free(ip); ip->block[12] = 0;
    } else if(writeblock(root, ptrbuf[0]) < 0) return -1;
    return 0;
  }
  logical -= n;
  if(logical < n*n){
    uint32 top = logical / n, leaf = logical % n;
    uint32 root = ip->block[13];
    if(root == 0 || readblock(root, ptrbuf[0]) < 0) return root ? -1 : 0;
    uint32 child = ptrbuf[0][top];
    if(child == 0 || readblock(child, ptrbuf[1]) < 0) return child ? -1 : 0;
    if(ptrbuf[1][leaf]){
      if(free_block(ptrbuf[1][leaf]) < 0) return -1;
      account_free(ip); ptrbuf[1][leaf] = 0;
    }
    if(ptr_empty(ptrbuf[1])){
      if(free_block(child) < 0) return -1;
      account_free(ip); ptrbuf[0][top] = 0;
    } else if(writeblock(child, ptrbuf[1]) < 0) return -1;
    if(ptr_empty(ptrbuf[0])){
      if(free_block(root) < 0) return -1;
      account_free(ip); ip->block[13] = 0;
    } else if(writeblock(root, ptrbuf[0]) < 0) return -1;
    return 0;
  }
  logical -= n*n;
  if(logical >= n*n*n) return -1;
  uint32 top = logical / (n*n), middle = (logical / n) % n, leaf = logical % n;
  uint32 root = ip->block[14];
  if(root == 0 || readblock(root, ptrbuf[0]) < 0) return root ? -1 : 0;
  uint32 midblock = ptrbuf[0][top];
  if(midblock == 0 || readblock(midblock, ptrbuf[1]) < 0)
    return midblock ? -1 : 0;
  uint32 leafblock = ptrbuf[1][middle];
  if(leafblock == 0 || readblock(leafblock, ptrbuf[2]) < 0)
    return leafblock ? -1 : 0;
  if(ptrbuf[2][leaf]){
    if(free_block(ptrbuf[2][leaf]) < 0) return -1;
    account_free(ip); ptrbuf[2][leaf] = 0;
  }
  if(ptr_empty(ptrbuf[2])){
    if(free_block(leafblock) < 0) return -1;
    account_free(ip); ptrbuf[1][middle] = 0;
  } else if(writeblock(leafblock, ptrbuf[2]) < 0) return -1;
  if(ptr_empty(ptrbuf[1])){
    if(free_block(midblock) < 0) return -1;
    account_free(ip); ptrbuf[0][top] = 0;
  } else if(writeblock(midblock, ptrbuf[1]) < 0) return -1;
  if(ptr_empty(ptrbuf[0])){
    if(free_block(root) < 0) return -1;
    account_free(ip); ip->block[14] = 0;
  } else if(writeblock(root, ptrbuf[0]) < 0) return -1;
  return 0;
}

static int tx_checkpoint(struct einode *ip);

static int
truncate_inode(struct einode *ip, uint64 newsize)
{
  uint64 maxsize = max_file_blocks() * e2.block_size;
  if(newsize > maxsize) return -1;
  if(newsize < ip->size){
    uint64 oldblocks = (ip->size + e2.block_size - 1) / e2.block_size;
    uint64 keepblocks = (newsize + e2.block_size - 1) / e2.block_size;
    while(oldblocks > keepblocks){
      if(clear_fileblock(ip, --oldblocks) < 0) return -1;
      // Keep the inode consistent with the blocks freed so far, so a split
      // transaction (tx_checkpoint) never commits dangling pointers.
      if(ip->size > oldblocks * e2.block_size)
        ip->size = oldblocks * e2.block_size;
      if(e2.journal && xj_space() <= 96 && tx_checkpoint(ip) < 0) return -1;
    }
    if(newsize && (newsize % e2.block_size)){
      uint32 bno = fileblock(ip, newsize / e2.block_size);
      if(bno){
        if(readblock(bno, blockbuf) < 0) return -1;
        memset(blockbuf + newsize % e2.block_size, 0,
               e2.block_size - newsize % e2.block_size);
        if(writeblock_data(bno, blockbuf) < 0) return -1;
      }
    }
  }
  ip->size = newsize;
  return writeinode(ip);
}

static int
nameeq(const uchar *p, int n, const char *s, int sn)
{
  if(n != sn) return 0;
  for(int i = 0; i < n; i++) if(p[i] != (uchar)s[i]) return 0;
  return 1;
}

static int
e2dirlookup(struct einode *dp, const char *name, int namelen, uint32 *ino)
{
  uint64 pos = 0, loaded = (uint64)-1;
  if((dp->mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -1;
  while(pos + 8 <= dp->size){
    uint64 logical = pos / e2.block_size;
    uint32 inblock = pos % e2.block_size;
    if(logical != loaded){
      uint32 b = fileblock(dp, logical);
      if(b == 0 || readblock(b, blockbuf) < 0) return -1;
      loaded = logical;
    }
    if(inblock + 8 > e2.block_size) return -1;
    uchar *de = blockbuf + inblock;
    uint32 deino = le32(de);
    uint16 reclen = le16(de + 4);
    uint8 nlen = de[6];
    if(reclen < 8 || (reclen & 3) || inblock + reclen > e2.block_size)
      return -1;
    if(deino && nlen <= reclen - 8 && nameeq(de + 8, nlen, name, namelen)){
      *ino = deino;
      return 0;
    }
    pos += reclen;
  }
  return -1;
}

static int
e2diradd(struct einode *dp, const char *name, int namelen, uint32 ino, uint8 type)
{
  uint32 need = (8 + namelen + 3) & ~3U;
  uint64 nblock = (dp->size + e2.block_size - 1) / e2.block_size;
  for(uint64 logical = 0; logical < nblock; logical++){
    uint32 bno = fileblock(dp, logical);
    if(bno == 0 || readblock(bno, blockbuf) < 0) return -1;
    for(uint32 pos = 0; pos + 8 <= e2.block_size; ){
      uchar *de = blockbuf + pos;
      uint32 deino = le32(de);
      uint16 reclen = le16(de + 4);
      uint8 nlen = de[6];
      if(reclen < 8 || (reclen & 3) || pos + reclen > e2.block_size)
        return -1;
      uint32 used = deino ? ((8 + nlen + 3) & ~3U) : 0;
      if(deino && nlen > reclen - 8) return -1;
      if((deino == 0 && reclen >= need) ||
         (deino != 0 && reclen >= used + need)){
        uint32 slot = pos;
        uint16 slotlen = reclen;
        if(deino){
          put16(de + 4, used);
          slot += used;
          slotlen -= used;
        }
        de = blockbuf + slot;
        memset(de, 0, slotlen);
        put32(de, ino);
        put16(de + 4, slotlen);
        de[6] = namelen;
        de[7] = type;
        memmove(de + 8, name, namelen);
        return writeblock(bno, blockbuf);
      }
      pos += reclen;
    }
  }

  if(nblock >= max_file_blocks()) return -1;
  uint32 bno = get_or_alloc_fileblock(dp, nblock);
  if(bno == 0) return -1;
  memset(blockbuf, 0, e2.block_size);
  put32(blockbuf, ino);
  put16(blockbuf + 4, e2.block_size);
  blockbuf[6] = namelen;
  blockbuf[7] = type;
  memmove(blockbuf + 8, name, namelen);
  if(writeblock(bno, blockbuf) < 0) return -1;
  dp->size = (uint64)(nblock + 1) * e2.block_size;
  return writeinode(dp);
}

static int
e2dirremove(struct einode *dp, const char *name, int namelen)
{
  uint64 pos = 0, loaded = (uint64)-1;
  uint32 bno = 0;
  while(pos + 8 <= dp->size){
    uint64 logical = pos / e2.block_size;
    uint32 inblock = pos % e2.block_size;
    if(logical != loaded){
      bno = fileblock(dp, logical);
      if(bno == 0 || readblock(bno, blockbuf) < 0) return -1;
      loaded = logical;
    }
    uchar *de = blockbuf + inblock;
    uint32 deino = le32(de);
    uint16 reclen = le16(de + 4);
    uint8 nlen = de[6];
    if(reclen < 8 || (reclen & 3) || inblock + reclen > e2.block_size)
      return -1;
    if(deino && nlen <= reclen - 8 && nameeq(de + 8, nlen, name, namelen)){
      put32(de, 0);
      return writeblock(bno, blockbuf);
    }
    pos += reclen;
  }
  return -1;
}

static int
e2dirempty(struct einode *dp)
{
  uint64 pos = 0, loaded = (uint64)-1;
  while(pos + 8 <= dp->size){
    uint64 logical = pos / e2.block_size;
    uint32 inblock = pos % e2.block_size;
    if(logical != loaded){
      uint32 bno = fileblock(dp, logical);
      if(bno == 0 || readblock(bno, blockbuf) < 0) return 0;
      loaded = logical;
    }
    uchar *de = blockbuf + inblock;
    uint32 deino = le32(de);
    uint16 reclen = le16(de + 4);
    uint8 nlen = de[6];
    if(reclen < 8 || (reclen & 3) || inblock + reclen > e2.block_size)
      return 0;
    if(deino && !(nlen == 1 && de[8] == '.') &&
       !(nlen == 2 && de[8] == '.' && de[9] == '.'))
      return 0;
    pos += reclen;
  }
  return 1;
}

static int
e2dirreplace(struct einode *dp, const char *name, int namelen, uint32 ino)
{
  uint64 pos = 0, loaded = (uint64)-1;
  uint32 bno = 0;
  while(pos + 8 <= dp->size){
    uint64 logical = pos / e2.block_size;
    uint32 inblock = pos % e2.block_size;
    if(logical != loaded){
      bno = fileblock(dp, logical);
      if(bno == 0 || readblock(bno, blockbuf) < 0) return -1;
      loaded = logical;
    }
    uchar *de = blockbuf + inblock;
    uint16 reclen = le16(de + 4);
    uint8 nlen = de[6];
    if(reclen < 8 || (reclen & 3) || inblock + reclen > e2.block_size)
      return -1;
    if(le32(de) && nlen <= reclen - 8 && nameeq(de + 8, nlen, name, namelen)){
      put32(de, ino);
      return writeblock(bno, blockbuf);
    }
    pos += reclen;
  }
  return -1;
}

static int
lookup(const char *path, uint32 *ino, struct einode *ip)
{
  const char *p = path, *start;
  uint32 curino = EXT2_ROOT_INO, nextino;
  struct einode cur;
  if(readinode(curino, &cur) < 0) return -1;
  while(*p == '/') p++;
  while(*p){
    start = p;
    while(*p && *p != '/') p++;
    if(e2dirlookup(&cur, start, p - start, &nextino) < 0) return -1;
    curino = nextino;
    if(readinode(curino, &cur) < 0) return -1;
    while(*p == '/') p++;
  }
  *ino = curino;
  *ip = cur;
  return 0;
}

static int
lookupparent(const char *path, struct einode *parent, char *name, int *namelen)
{
  char buf[MAXPATH];
  int n = strlen(path);
  if(n <= 0 || n >= MAXPATH) return -1;
  safestrcpy(buf, path, sizeof(buf));
  while(n > 1 && buf[n-1] == '/') buf[--n] = 0;
  char *last = buf + n;
  while(last > buf && last[-1] != '/') last--;
  if(*last == 0) return -1;
  *namelen = strlen(last);
  if(*namelen <= 0 || *namelen > EXT2_NAME_MAX) return -1;
  memmove(name, last, *namelen);
  name[*namelen] = 0;
  if(last == buf){
    buf[0] = '/'; buf[1] = 0;
  } else if(last == buf + 1){
    buf[1] = 0;
  } else {
    last[-1] = 0;
  }
  uint32 pino;
  if(lookup(buf, &pino, parent) < 0 ||
     (parent->mode & EXT2_S_IFMT) != EXT2_S_IFDIR)
    return -1;
  return 0;
}

static int
setup_ext2(uint32 start, uint32 sectors, int partno)
{
  uchar sb[1024];
  uint32 compat, incompat, rocompat;
  e2.part_lba = start;
  e2.part_sectors = sectors;
  if(diskbytes(1024, sb, sizeof(sb)) < 0 || le16(sb + 56) != EXT2_MAGIC){
    printf("ext2: p%d has no ext superblock\n", partno); return -1;
  }
  compat = le32(sb + 92);
  incompat = le32(sb + 96);
  rocompat = le32(sb + 100);
  if(compat & 4U){
    printf("ext2: p%d has a journal; use mke2fs -t ext2\n", partno);
    return -1;
  }
  // FILETYPE (0x2) is understood. Reject extents, 64-bit block numbers and
  // other layouts before interpreting an ext4 volume as ext2.
  if(incompat & ~2U){
    printf("ext2: p%d unsupported incompat features=0x%x\n", partno, incompat);
    return -1;
  }
  if(rocompat & ~3U){
    printf("ext2: p%d unsupported ro-compat features=0x%x\n", partno, rocompat);
    return -1;
  }
  e2.block_size = 1024U << le32(sb + 24);
  if(e2.block_size < 1024 || e2.block_size > sizeof(blockbuf)){
    printf("ext2: p%d unsupported block size %d\n", partno, e2.block_size); return -1;
  }
  e2.inodes = le32(sb);
  e2.blocks = le32(sb + 4);
  e2.first_data_block = le32(sb + 20);
  e2.first_ino = le32(sb + 76) ? le32(sb + 84) : 11;
  if(e2.blocks == 0 || e2.inodes == 0 ||
     (uint64)e2.blocks * e2.block_size > (uint64)sectors * SECTOR){
    printf("ext2: p%d geometry exceeds partition\n", partno);
    return -1;
  }
  e2.blocks_per_group = le32(sb + 32);
  e2.inodes_per_group = le32(sb + 40);
  e2.inode_size = le32(sb + 76) ? le16(sb + 88) : 128;
  if(e2.inode_size < 128 || e2.inode_size > sizeof(inodebuf) ||
     e2.inodes_per_group == 0 || e2.blocks_per_group == 0 ||
     e2.inodes_per_group > e2.block_size * 8 ||
     e2.blocks_per_group > e2.block_size * 8)
    return -1;
  e2.desc_size = 32;
  e2.groups = (e2.blocks - e2.first_data_block +
               e2.blocks_per_group - 1) / e2.blocks_per_group;
  e2.gd_block = e2.block_size == 1024 ? 2 : 1;
  e2.ready = 1;
  e2.dirty = 0;
  printf("ext2: p%d ready rw start=%d sectors=%d block=%d groups=%d\n",
         partno, start, sectors, e2.block_size, e2.groups);
  return 0;
}

void
ext2init(void)
{
  uchar mbr[512];
  int found = 0;
  initsleeplock(&e2.lock, "ext2");
  e2.ready = 0;
  // External journal: the unused tail of the raw xv6 root partition.  It must
  // be recovered before any ext2 metadata is interpreted.
  uint32 rlba, rsec, jstart = FSSIZE * (BSIZE / SECTOR);
  if(rootdev_raw_info(&rlba, &rsec) == 0 && rsec > jstart &&
     xj_init(rlba + jstart, rsec - jstart) == 0)
    e2.journal = 1;
  else
    printf("ext2: no raw xv6 root partition; external journal disabled\n");
  if(sdsector(0, mbr, 0) < 0) return;
  if(mbr[510] != 0x55 || mbr[511] != 0xaa){
    printf("ext2: no MBR; disabled\n");
    return;
  }
  // rootfstype=ext2 root=...pN names the partition; try it first.
  int pref = rootdev_ext2_part();
  if(pref >= 1 && pref <= 4){
    uchar *p = mbr + 446 + (pref - 1)*16;
    if(le32(p + 8) != 0 && setup_ext2(le32(p + 8), le32(p + 12), pref) == 0)
      return;
    printf("ext2: root partition p%d has no usable ext2\n", pref);
  }
  // Try every Linux partition. This permits p2 to remain ext4 while a simple
  // ext2 filesystem is placed in p3.
  for(int i = 0; i < 4; i++){
    uchar *p = mbr + 446 + i*16;
    if(p[4] == 0x83 && le32(p + 8) != 0){
      found = 1;
      if(setup_ext2(le32(p + 8), le32(p + 12), i + 1) == 0) return;
    }
  }
  if(!found) printf("ext2: no Linux partition; disabled\n");
  else printf("ext2: no compatible ext2 partition; disabled\n");
}

int
ext2ready(void)
{
  return e2.ready;
}

// First card sector of the partition the driver mounted (0 if none), so
// do_mounts.c can check that /dev/root names the same partition.
uint32
ext2_part_lba(void)
{
  return e2.ready ? e2.part_lba : 0;
}

// ---------------------------------------------------------------------------
// Transactions.  With the external journal every mutating operation is one
// xjournal transaction: metadata sectors are captured, data sectors are
// written directly first (ordered mode), and a failed operation is aborted so
// nothing it changed reaches the disk.  Without the journal (no raw xv6 root
// partition) writes go straight to the card as before.

static void
tx_begin(void)
{
  e2.nfreed = 0;
  if(e2.journal)
    xj_begin();
}

static int
tx_end(int ok)
{
  e2.nfreed = 0;
  if(!e2.journal)
    return ok ? 0 : -1;
  if(!ok){
    xj_abort();
    return -1;
  }
  return xj_commit();
}

// Very large truncates or writes touch more metadata than one transaction
// holds.  At a consistent point (the inode written with a matching size) the
// work so far is committed and a new transaction continues.
static int
tx_checkpoint(struct einode *ip)
{
  if(!e2.journal || xj_space() > 96)
    return 0;
  if(writeinode(ip) < 0 || xj_commit() < 0)
    return -1;
  e2.nfreed = 0;
  xj_begin();
  return 0;
}

// ---------------------------------------------------------------------------
// Open-inode table.  VFS vnodes for ext2 files are bound to an inode number at
// open time, so I/O on an open descriptor never re-resolves the path.  An
// inode whose last name is removed while open becomes an orphan: its blocks
// are released when the last descriptor closes, as in Unix.

static int
open_slot(uint32 ino)
{
  for(int i = 0; i < NFILE; i++)
    if(e2.open[i].ref > 0 && e2.open[i].ino == ino)
      return i;
  return -1;
}

// Drop one link from an inode whose directory entry is already gone.
static int
drop_link(struct einode *ip)
{
  int directory = (ip->mode & EXT2_S_IFMT) == EXT2_S_IFDIR;
  if(directory)
    ip->links = 0;
  else if(ip->links)
    ip->links--;
  if(ip->links)
    return writeinode(ip);
  int s = open_slot(ip->ino);
  if(s >= 0){
    e2.open[s].orphan = 1;
    return writeinode(ip);       // links=0 on disk, blocks kept until close
  }
  if(truncate_inode(ip, 0) < 0)
    return -1;
  return free_inode(ip->ino, directory);
}

int
ext2open(char *path, uint64 *handle)
{
  struct einode ip;
  uint32 ino;
  int free = -1;
  acquiresleep(&e2.lock);
  if(lookup(path, &ino, &ip) < 0){
    releasesleep(&e2.lock);
    return -1;
  }
  int s = open_slot(ino);
  if(s < 0){
    for(int i = 0; i < NFILE; i++)
      if(e2.open[i].ref == 0){ free = i; break; }
    if(free < 0){
      releasesleep(&e2.lock);
      return -1;
    }
    s = free;
    e2.open[s].ino = ino;
    e2.open[s].orphan = 0;
  }
  e2.open[s].ref++;
  *handle = ino;
  releasesleep(&e2.lock);
  return 0;
}

void
ext2release(uint64 handle)
{
  struct einode ip;
  acquiresleep(&e2.lock);
  int s = open_slot((uint32)handle);
  if(s < 0)
    panic("ext2release");
  if(--e2.open[s].ref == 0 && e2.open[s].orphan){
    e2.open[s].orphan = 0;
    tx_begin();
    int ok = readinode((uint32)handle, &ip) == 0 && ip.links == 0 &&
             truncate_inode(&ip, 0) == 0 &&
             free_inode(ip.ino, (ip.mode & EXT2_S_IFMT) == EXT2_S_IFDIR) == 0;
    if(tx_end(ok) < 0)
      printf("ext2: orphan inode %d not reclaimed; run e2fsck\n", (int)handle);
  }
  releasesleep(&e2.lock);
}

// ---------------------------------------------------------------------------
// Reads.

static int
read_locked(struct einode *ip, uint64 off, void *dst, int n)
{
  int done = 0;
  if(n < 0 || (ip->mode & EXT2_S_IFMT) != EXT2_S_IFREG) return -1;
  if(off >= ip->size) return 0;
  if((uint64)n > ip->size - off) n = ip->size - off;
  while(done < n){
    uint64 logical = off / e2.block_size;
    uint32 boff = off % e2.block_size;
    int take = e2.block_size - boff;
    uint32 b = fileblock(ip, logical);
    if(take > n - done) take = n - done;
    if(b == 0) memset(blockbuf, 0, e2.block_size);
    else if(readblock(b, blockbuf) < 0) return -1;
    memmove((uchar*)dst + done, blockbuf + boff, take);
    off += take; done += take;
  }
  return done;
}

int
ext2readfile(char *path, uint64 off, void *dst, int n)
{
  struct einode ip;
  uint32 ino;
  int r = -1;
  acquiresleep(&e2.lock);
  if(lookup(path, &ino, &ip) == 0)
    r = read_locked(&ip, off, dst, n);
  releasesleep(&e2.lock);
  return r;
}

int
ext2readino(uint64 handle, uint64 off, void *dst, int n)
{
  struct einode ip;
  int r = -1;
  acquiresleep(&e2.lock);
  if(readinode((uint32)handle, &ip) == 0)
    r = read_locked(&ip, off, dst, n);
  releasesleep(&e2.lock);
  return r;
}

static void
fill_stat(struct einode *ip, uint *ino, ushort *mode, uint64 *size)
{
  *ino = ip->ino;
  *mode = ip->mode;
  *size = ip->size;
}

int
ext2stat(char *path, uint *ino, ushort *mode, uint64 *size)
{
  struct einode ip;
  uint32 inum;
  int r = -1;
  acquiresleep(&e2.lock);
  if(lookup(path, &inum, &ip) == 0){
    fill_stat(&ip, ino, mode, size);
    r = 0;
  }
  releasesleep(&e2.lock);
  return r;
}

int
ext2statino(uint64 handle, uint *ino, ushort *mode, uint64 *size)
{
  struct einode ip;
  int r = -1;
  acquiresleep(&e2.lock);
  if(readinode((uint32)handle, &ip) == 0){
    fill_stat(&ip, ino, mode, size);
    r = 0;
  }
  releasesleep(&e2.lock);
  return r;
}

int
ext2readdir(char *path, int index, struct ext2_user_dirent *out)
{
  struct einode dp, child;
  uint32 ino;
  uint64 pos = 0, loaded = (uint64)-1;
  int seen = 0;
  if(index < 0) return -1;
  acquiresleep(&e2.lock);
  if(lookup(path, &ino, &dp) < 0 || (dp.mode & EXT2_S_IFMT) != EXT2_S_IFDIR){
    releasesleep(&e2.lock); return -1;
  }
  while(pos + 8 <= dp.size){
    uint64 logical = pos / e2.block_size;
    uint32 inblock = pos % e2.block_size;
    if(logical != loaded){
      uint32 b = fileblock(&dp, logical);
      if(b == 0 || readblock(b, blockbuf) < 0) break;
      loaded = logical;
    }
    uchar *de = blockbuf + inblock;
    uint32 deino = le32(de);
    uint16 reclen = le16(de + 4);
    uint8 nlen = de[6];
    uint8 type = de[7];
    if(reclen < 8 || (reclen & 3) || inblock + reclen > e2.block_size) break;
    if(deino && nlen <= reclen - 8){
      if(seen++ == index){
        memset(out, 0, sizeof(*out));
        out->inode = deino; out->type = type;
        memmove(out->name, de + 8, nlen);
        out->name[nlen] = 0;
        if(readinode(deino, &child) == 0){ out->size = child.size; out->mode = child.mode; }
        releasesleep(&e2.lock); return 1;
      }
    }
    pos += reclen;
  }
  releasesleep(&e2.lock);
  return 0;
}

// ---------------------------------------------------------------------------
// Mutations.  Each public entry point is lock + transaction + *_locked().

static int
create_locked(char *path)
{
  struct einode parent, existing, child;
  uint32 ino;
  char name[EXT2_NAME_MAX + 1];
  int namelen;
  if(!e2.ready || lookup(path, &ino, &existing) == 0 ||
     lookupparent(path, &parent, name, &namelen) < 0 ||
     mark_write_session_dirty() < 0)
    return -1;
  uint32 group = (parent.ino - 1) / e2.inodes_per_group;
  if(alloc_inode(group, EXT2_S_IFREG | 0644, &child) < 0 ||
     e2diradd(&parent, name, namelen, child.ino, 1) < 0)
    return -1;               // the aborted transaction also undoes alloc_inode
  return 0;
}

int
ext2createfile(char *path)
{
  acquiresleep(&e2.lock);
  tx_begin();
  int r = tx_end(create_locked(path) == 0);
  releasesleep(&e2.lock);
  return r;
}

static int
truncate_locked(struct einode *ip, uint64 size)
{
  if((ip->mode & EXT2_S_IFMT) != EXT2_S_IFREG ||
     mark_write_session_dirty() < 0)
    return -1;
  return truncate_inode(ip, size);
}

int
ext2truncatefile(char *path, uint64 size)
{
  struct einode ip;
  uint32 ino;
  acquiresleep(&e2.lock);
  tx_begin();
  int ok = lookup(path, &ino, &ip) == 0 && truncate_locked(&ip, size) == 0;
  int r = tx_end(ok);
  releasesleep(&e2.lock);
  return r;
}

int
ext2truncino(uint64 handle, uint64 size)
{
  struct einode ip;
  acquiresleep(&e2.lock);
  tx_begin();
  int ok = readinode((uint32)handle, &ip) == 0 &&
           truncate_locked(&ip, size) == 0;
  int r = tx_end(ok);
  releasesleep(&e2.lock);
  return r;
}

// Returns bytes written; on a partial failure the completed prefix is kept.
static int
write_locked(struct einode *ip, uint64 off, int user_src, uint64 src, int n)
{
  int done = 0;
  uint64 maxsize = max_file_blocks() * e2.block_size;
  if(n < 0 || off + n < off || off + n > maxsize ||
     (ip->mode & EXT2_S_IFMT) != EXT2_S_IFREG ||
     mark_write_session_dirty() < 0)
    return -1;
  while(done < n){
    uint64 logical = off / e2.block_size;
    uint32 boff = off % e2.block_size;
    int take = e2.block_size - boff;
    if(take > n - done) take = n - done;
    if(tx_checkpoint(ip) < 0) break;
    uint32 bno = get_or_alloc_fileblock(ip, logical);
    if(bno == 0 || readblock(bno, blockbuf) < 0 ||
       either_copyin(blockbuf + boff, user_src, src + done, take) < 0 ||
       writeblock_data(bno, blockbuf) < 0)
      break;
    off += take;
    done += take;
    if(off > ip->size) ip->size = off;
  }
  if(done == 0)
    return -1;
  return writeinode(ip) < 0 ? -1 : done;
}

static int
write_common(struct einode *ip, uint64 off, int user_src, uint64 src, int n)
{
  int r = write_locked(ip, off, user_src, src, n);
  if(tx_end(r > 0) < 0)
    return -1;
  return r;
}

int
ext2writefile(char *path, struct fat32_file *ignored, uint64 off,
              int user_src, uint64 src, int n)
{
  struct einode ip;
  uint32 ino;
  (void)ignored;
  if(n == 0) return 0;
  acquiresleep(&e2.lock);
  tx_begin();
  int r;
  if(lookup(path, &ino, &ip) < 0){
    tx_end(0);
    r = -1;
  } else
    r = write_common(&ip, off, user_src, src, n);
  releasesleep(&e2.lock);
  return r;
}

int
ext2writeino(uint64 handle, uint64 off, int user_src, uint64 src, int n)
{
  struct einode ip;
  if(n == 0) return 0;
  acquiresleep(&e2.lock);
  tx_begin();
  int r;
  if(readinode((uint32)handle, &ip) < 0){
    tx_end(0);
    r = -1;
  } else
    r = write_common(&ip, off, user_src, src, n);
  releasesleep(&e2.lock);
  return r;
}

static int
mkdir_locked(char *path)
{
  struct einode parent, existing, child;
  uint32 ino, bno;
  char name[EXT2_NAME_MAX + 1];
  int namelen;
  if(!e2.ready || lookup(path, &ino, &existing) == 0 ||
     lookupparent(path, &parent, name, &namelen) < 0 ||
     mark_write_session_dirty() < 0)
    return -1;
  uint32 group = (parent.ino - 1) / e2.inodes_per_group;
  if(alloc_inode(group, EXT2_S_IFDIR | 0755, &child) < 0)
    return -1;
  child.links = 2;
  bno = get_or_alloc_fileblock(&child, 0);
  if(bno == 0)
    return -1;
  memset(blockbuf, 0, e2.block_size);
  put32(blockbuf, child.ino);
  put16(blockbuf + 4, 12);
  blockbuf[6] = 1; blockbuf[7] = 2; blockbuf[8] = '.';
  put32(blockbuf + 12, parent.ino);
  put16(blockbuf + 16, e2.block_size - 12);
  blockbuf[18] = 2; blockbuf[19] = 2;
  blockbuf[20] = '.'; blockbuf[21] = '.';
  child.size = e2.block_size;
  if(writeblock(bno, blockbuf) < 0 || writeinode(&child) < 0 ||
     e2diradd(&parent, name, namelen, child.ino, 2) < 0)
    return -1;
  if(readinode(parent.ino, &parent) < 0)   // e2diradd may have grown it
    return -1;
  parent.links++;
  return writeinode(&parent);
}

int
ext2mkdir(char *path)
{
  acquiresleep(&e2.lock);
  tx_begin();
  int r = tx_end(mkdir_locked(path) == 0);
  releasesleep(&e2.lock);
  return r;
}

static int
unlink_locked(char *path)
{
  struct einode parent, child;
  uint32 ino;
  char name[EXT2_NAME_MAX + 1];
  int namelen;
  if(lookupparent(path, &parent, name, &namelen) < 0 ||
     (namelen == 1 && name[0] == '.') ||
     (namelen == 2 && name[0] == '.' && name[1] == '.') ||
     e2dirlookup(&parent, name, namelen, &ino) < 0 ||
     readinode(ino, &child) < 0 || child.links == 0)
    return -1;
  int directory = (child.mode & EXT2_S_IFMT) == EXT2_S_IFDIR;
  if((directory && !e2dirempty(&child)) ||
     mark_write_session_dirty() < 0 ||
     e2dirremove(&parent, name, namelen) < 0)
    return -1;
  if(directory){
    if(readinode(parent.ino, &parent) < 0) return -1;
    if(parent.links) parent.links--;     // child's ".." no longer counts
    if(writeinode(&parent) < 0) return -1;
  }
  return drop_link(&child);
}

int
ext2unlink(char *path)
{
  acquiresleep(&e2.lock);
  tx_begin();
  int r = tx_end(unlink_locked(path) == 0);
  releasesleep(&e2.lock);
  return r;
}

// Is directory `dir` equal to, or an ancestor of, directory `ino`?
static int
is_ancestor(uint32 dir, uint32 ino)
{
  struct einode cur;
  for(uint32 steps = 0; steps < e2.inodes; steps++){
    if(ino == dir) return 1;
    if(ino == EXT2_ROOT_INO) return 0;
    uint32 up;
    if(readinode(ino, &cur) < 0 || e2dirlookup(&cur, "..", 2, &up) < 0)
      return 1;                          // be conservative on a damaged tree
    if(up == ino) return 0;
    ino = up;
  }
  return 1;
}

static int
rename_locked(char *oldpath, char *newpath)
{
  struct einode oldparent, newparent, child, target;
  uint32 ino, tino;
  char oldname[EXT2_NAME_MAX + 1], newname[EXT2_NAME_MAX + 1];
  int oldlen, newlen;
  if(lookupparent(oldpath, &oldparent, oldname, &oldlen) < 0 ||
     lookupparent(newpath, &newparent, newname, &newlen) < 0 ||
     (oldlen == 1 && oldname[0] == '.') ||
     (oldlen == 2 && oldname[0] == '.' && oldname[1] == '.') ||
     (newlen == 1 && newname[0] == '.') ||
     (newlen == 2 && newname[0] == '.' && newname[1] == '.') ||
     e2dirlookup(&oldparent, oldname, oldlen, &ino) < 0 ||
     readinode(ino, &child) < 0)
    return -1;
  int isdir = (child.mode & EXT2_S_IFMT) == EXT2_S_IFDIR;
  // A directory cannot be moved below itself: that would detach a cycle.
  if(isdir && is_ancestor(child.ino, newparent.ino))
    return -1;
  int exists = e2dirlookup(&newparent, newname, newlen, &tino) == 0;
  if(exists && tino == child.ino)
    return 0;                            // same object: POSIX no-op
  if(exists){
    if(readinode(tino, &target) < 0) return -1;
    int tdir = (target.mode & EXT2_S_IFMT) == EXT2_S_IFDIR;
    if(isdir != tdir || (tdir && !e2dirempty(&target)))
      return -1;
  }
  if(mark_write_session_dirty() < 0)
    return -1;
  // Point the new name at the child (replacing any target), then drop the old
  // name.  With the journal the whole sequence commits atomically.
  if(exists){
    if(e2dirreplace(&newparent, newname, newlen, child.ino) < 0) return -1;
  } else if(e2diradd(&newparent, newname, newlen, child.ino, isdir ? 2 : 1) < 0)
    return -1;
  if(readinode(oldparent.ino, &oldparent) < 0 ||
     e2dirremove(&oldparent, oldname, oldlen) < 0)
    return -1;
  if(isdir && oldparent.ino != newparent.ino){
    if(e2dirreplace(&child, "..", 2, newparent.ino) < 0) return -1;
    if(readinode(oldparent.ino, &oldparent) < 0) return -1;
    if(oldparent.links) oldparent.links--;
    if(writeinode(&oldparent) < 0) return -1;
    if(readinode(newparent.ino, &newparent) < 0) return -1;
    newparent.links++;
    if(writeinode(&newparent) < 0) return -1;
  }
  if(exists){
    if((target.mode & EXT2_S_IFMT) == EXT2_S_IFDIR){
      if(readinode(newparent.ino, &newparent) < 0) return -1;
      if(newparent.links) newparent.links--;   // replaced dir's ".."
      if(writeinode(&newparent) < 0) return -1;
    }
    if(readinode(tino, &target) < 0 || drop_link(&target) < 0)
      return -1;
  }
  return 0;
}

int
ext2rename(char *oldpath, char *newpath)
{
  if(strncmp(oldpath, newpath, MAXPATH) == 0) return 0;
  acquiresleep(&e2.lock);
  tx_begin();
  int r = tx_end(rename_locked(oldpath, newpath) == 0);
  releasesleep(&e2.lock);
  return r;
}

int
ext2fsync(void)
{
  int r;
  acquiresleep(&e2.lock);
  // Each operation already committed synchronously (journal) or wrote its
  // sectors in place; sdflush is the card-level completion barrier.
  r = sdflush();
  releasesleep(&e2.lock);
  return r;
}
