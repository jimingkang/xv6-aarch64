// Small ext2 filesystem backend for the VFS mount at /mnt/ext2.
//
// It supports classic ext2 regular files, linear directories and all three
// levels of indirect blocks.  Every mounted device has its own ext2_fs and
// sleeplock; ordinary device I/O goes through bread()/bwrite().
//
// ext2 itself has no journal.  When a raw xv6 root partition exists, every
// mutating operation becomes one transaction in an external journal kept in
// p2 after the xv6 image (kernel/fs/ext2/xjournal.c): metadata is captured and
// committed atomically, file data is written in place before the commit.

#include <kernel/types.h>
#include "aarch64.h"
#include <kernel/defs.h>
#include <kernel/param.h>
#include <kernel/spinlock.h>
#include <kernel/sleeplock.h>
#include <kernel/fs.h>
#include <kernel/buf.h>
#include <kernel/ext2.h>
#include <kernel/fat32.h>
#include <kernel/stat.h>
#include <kernel/file.h>
#include <kernel/device.h>
#include <kernel/vfs.h>
#include <kernel/module.h>
#include <kernel/blkdev.h>

#define SECTOR 512
#define EXT2_MAGIC 0xef53
#define EXT2_ROOT_INO 2
#define EXT2_S_IFMT  0xf000
#define EXT2_S_IFDIR 0x4000
#define EXT2_S_IFREG 0x8000

#define EXT2_FEATURE_COMPAT_HAS_JOURNAL 0x0004
#define EXT2_FEATURE_COMPAT_EXT_ATTR    0x0008
#define EXT2_FEATURE_COMPAT_RESIZE_INO  0x0010
#define EXT2_FEATURE_COMPAT_DIR_INDEX   0x0020
#define EXT2_FEATURE_INCOMPAT_FILETYPE  0x0002
#define EXT2_FEATURE_RO_SPARSE_SUPER    0x0001
#define EXT2_FEATURE_RO_LARGE_FILE      0x0002

// Layout-changing inode flags this small driver cannot interpret safely.
#define EXT2_COMPR_FL       0x00000004
#define EXT2_INDEX_FL       0x00001000
#define EXT4_EXTENTS_FL     0x00080000
#define EXT4_INLINE_DATA_FL 0x10000000

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

struct ext2_fs {
  struct sleeplock lock;
  int ready;
  int dirty;
  int was_unclean;       // mounted with EXT2_VALID_FS already clear
  int journal;          // external xjournal transactions enabled
  int direct;           // current write bypasses the journal (file data)
  uint32 nfreed;        // blocks freed in the current transaction
  uint32 freed[E2_MAXFREED];
  struct { uint32 ino; int ref; int orphan; } open[NFILE];
  int dev;              // block device of the partition (mmcblk0pN)
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
  uchar sectorbuf[SECTOR];
  uchar inodebuf[512];
  uchar *blockbuf;
  // Scratch pages are private to this mounted instance and protected by
  // lock.  Three independent pages are needed for a triple-indirect path.
  uint32 *ptrbuf[3];
};

static struct ext2_fs ext2_instances[NSUPER];
static struct sleeplock ext2_mount_lock;
static int xj_owner_dev;
static struct inode_ops ext2_iops;
static struct inode_ops ext2_file_iops;
static struct inode_ops ext2_dir_iops;
static const struct file_operations ext2_file_fops;
static const struct file_operations ext2_dir_fops;

static struct ext2_fs*
ext2_by_dev(int dev)
{
  if(dev <= 0 || dev >= NSUPER)
    return 0;
  return &ext2_instances[dev];
}

static struct ext2_fs*
ext2_from_inode(struct inode *ip)
{
  struct super_block *sb = ip ? getsuper(ip->dev) : 0;
  return sb && sb->priv ? sb->priv : 0;
}

// The two legacy ext2-only system calls predate VFS mounts.  Keep them bound
// to the first mounted ext2 instance; normal file operations always select
// the instance from ip->dev and super_block.priv.
static struct ext2_fs*
ext2_default(void)
{
  for(int dev = 1; dev < NSUPER; dev++)
    if(ext2_instances[dev].ready)
      return &ext2_instances[dev];
  return 0;
}

static uint16 le16(const uchar *p) { return p[0] | ((uint16)p[1] << 8); }
static uint32 le32(const uchar *p) {
  return p[0] | ((uint32)p[1] << 8) | ((uint32)p[2] << 16) |
         ((uint32)p[3] << 24);
}
static void put16(uchar *p, uint16 v) { p[0] = v; p[1] = v >> 8; }
static void put32(uchar *p, uint32 v) {
  p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

// The external journal remains sector based.  Reads first consult its
// uncommitted overlay, then obtain the containing filesystem block via bread.
static int
e2_rsector(struct ext2_fs *fs, uint32 lba, void *buf)
{
  if(fs->journal && xj_read(lba, buf))
    return 0;
  uint32 rel = lba - fs->part_lba;
  uint32 bsize = blkdev_bsize(fs->dev);
  uint64 byte = (uint64)rel * SECTOR;
  if(bsize < SECTOR || byte / bsize > 0xffffffffU)
    return -1;
  struct buf *bp = bread(fs->dev, (uint32)(byte / bsize));
  if(bp->error){
    brelse(bp);
    return -1;
  }
  memmove(buf, bp->data + byte % bsize, SECTOR);
  brelse(bp);
  return 0;
}

// Write one contiguous range through its cached filesystem block.  A 4 KiB
// ext2 data block therefore causes one device write, not eight 512-byte
// read/modify/write cycles.
static int
e2_cache_write(struct ext2_fs *fs, uint64 byte, const void *src, uint32 n)
{
  uint32 bsize = blkdev_bsize(fs->dev);
  if(bsize < SECTOR)
    return -1;
  if(n > bsize - byte % bsize)
    return -1;
  if(byte / bsize > 0xffffffffU)
    return -1;
  struct buf *bp = bread(fs->dev, (uint32)(byte / bsize));
  if(bp->error){
    brelse(bp);
    return -1;
  }
  memmove(bp->data + byte % bsize, src, n);
  bwrite(bp);
  int error = bp->error;
  brelse(bp);
  return error ? -1 : 0;
}

static int
diskbytes(struct ext2_fs *fs, uint64 byte, void *dst, uint32 n)
{
  uchar *p = dst;
  if(byte + n < byte || byte + n > (uint64)fs->part_sectors * SECTOR)
    return -1;
  while(n){
    uint32 lba = fs->part_lba + byte / SECTOR;
    uint32 off = byte % SECTOR;
    uint32 take = SECTOR - off;
    if(take > n) take = n;
    if(e2_rsector(fs, lba, fs->sectorbuf) < 0) return -1;
    memmove(p, fs->sectorbuf + off, take);
    p += take; byte += take; n -= take;
  }
  return 0;
}

static int
diskbytes_write(struct ext2_fs *fs, uint64 byte, const void *src, uint32 n)
{
  const uchar *p = src;
  if(byte + n < byte || byte + n > (uint64)fs->part_sectors * SECTOR)
    return -1;
  while(n){
    uint32 bsize = blkdev_bsize(fs->dev);
    if(bsize < SECTOR)
      return -1;
    uint32 block_take = bsize - byte % bsize;
    if(block_take > n) block_take = n;

    // Normal writes, including ordered file data, modify a cached ext2 block
    // and write it once.  If a data write overlaps a sector already captured
    // by the current transaction, preserve the old sector-level rule and
    // update that journal copy instead.
    int journal_range = fs->journal && xj_active() && !fs->direct;
    if(fs->journal && xj_active() && fs->direct){
      uint64 first = byte / SECTOR;
      uint64 last = (byte + block_take - 1) / SECTOR;
      for(uint64 s = first; s <= last; s++)
        if(xj_contains(fs->part_lba + s)){
          journal_range = 1;
          break;
        }
    }
    if(!journal_range){
      if(e2_cache_write(fs, byte, p, block_take) < 0)
        return -1;
      p += block_take;
      byte += block_take;
      n -= block_take;
      continue;
    }

    // Metadata stays outside the home bcache until commit.  Capture complete
    // 512-byte sectors in xjournal, overlaying an earlier write when present.
    uint32 lba = fs->part_lba + byte / SECTOR;
    uint32 off = byte % SECTOR;
    uint32 take = SECTOR - off;
    if(take > block_take) take = block_take;
    if(off == 0 && take == SECTOR){
      if(xj_write(lba, p) < 0) return -1;
    } else {
      if(e2_rsector(fs, lba, fs->sectorbuf) < 0) return -1;
      memmove(fs->sectorbuf + off, p, take);
      if(xj_write(lba, fs->sectorbuf) < 0) return -1;
    }
    p += take; byte += take; n -= take;
  }
  return 0;
}

static int
readblock(struct ext2_fs *fs, uint32 bno, void *dst)
{
  if(bno == 0 || bno >= fs->blocks) return -1;
  return diskbytes(fs, (uint64)bno * fs->block_size, dst, fs->block_size);
}

static int
writeblock(struct ext2_fs *fs, uint32 bno, const void *src)
{
  if(bno == 0 || bno >= fs->blocks) return -1;
  return diskbytes_write(fs, (uint64)bno * fs->block_size, src, fs->block_size);
}

// File contents bypass the journal (ordered mode): they reach the card before
// the transaction that makes them reachable commits.
static int
writeblock_data(struct ext2_fs *fs, uint32 bno, const void *src)
{
  fs->direct = 1;
  int r = writeblock(fs, bno, src);
  fs->direct = 0;
  return r;
}

static int
readgd(struct ext2_fs *fs, uint32 group, uchar *gd)
{
  if(group >= fs->groups || fs->desc_size > sizeof(fs->inodebuf)) return -1;
  return diskbytes(fs, (uint64)fs->gd_block * fs->block_size +
                   group * fs->desc_size, gd, fs->desc_size);
}

static int
writegd(struct ext2_fs *fs, uint32 group, uchar *gd)
{
  if(group >= fs->groups || fs->desc_size > sizeof(fs->inodebuf)) return -1;
  return diskbytes_write(fs, (uint64)fs->gd_block * fs->block_size +
                         group * fs->desc_size, gd, fs->desc_size);
}

static int
readinode(struct ext2_fs *fs, uint32 ino, struct einode *ip)
{
  uint32 group, index, table;
  uint64 gd_off, off;
  if(!fs->ready || ino == 0 || ino > fs->inodes) return -1;
  group = (ino - 1) / fs->inodes_per_group;
  index = (ino - 1) % fs->inodes_per_group;
  if(group >= fs->groups || fs->inode_size > sizeof(fs->inodebuf)) return -1;
  gd_off = (uint64)fs->gd_block * fs->block_size + group * fs->desc_size;
  if(diskbytes(fs, gd_off, fs->inodebuf, fs->desc_size) < 0) return -1;
  table = le32(fs->inodebuf + 8);
  off = (uint64)table * fs->block_size + (uint64)index * fs->inode_size;
  if(diskbytes(fs, off, fs->inodebuf, fs->inode_size) < 0) return -1;
  ip->ino = ino;
  ip->mode = le16(fs->inodebuf);
  ip->links = le16(fs->inodebuf + 26);
  ip->size = le32(fs->inodebuf + 4);
  if((ip->mode & EXT2_S_IFMT) == EXT2_S_IFREG)
    ip->size |= (uint64)le32(fs->inodebuf + 108) << 32;
  ip->sectors = le32(fs->inodebuf + 28);
  ip->flags = le32(fs->inodebuf + 32);
  for(int i = 0; i < 15; i++) ip->block[i] = le32(fs->inodebuf + 40 + i*4);
  return 0;
}

static int
inode_supported(struct einode *ip)
{
  uint16 kind = ip->mode & EXT2_S_IFMT;
  if(kind != EXT2_S_IFREG && kind != EXT2_S_IFDIR)
    return -1;
  if(ip->flags & (EXT2_COMPR_FL | EXT4_EXTENTS_FL | EXT4_INLINE_DATA_FL))
    return -1;
  // htree directories require indexed lookup/insertion and cannot safely be
  // treated as the linear ext2_dir_entry_2 stream implemented below.
  if(kind == EXT2_S_IFDIR && (ip->flags & EXT2_INDEX_FL))
    return -1;
  return 0;
}

static int
writeinode(struct ext2_fs *fs, struct einode *ip)
{
  uint32 group, index, table;
  uint64 off;
  if(ip->ino == 0 || ip->ino > fs->inodes) return -1;
  group = (ip->ino - 1) / fs->inodes_per_group;
  index = (ip->ino - 1) % fs->inodes_per_group;
  if(readgd(fs, group, fs->inodebuf) < 0) return -1;
  table = le32(fs->inodebuf + 8);
  off = (uint64)table * fs->block_size + (uint64)index * fs->inode_size;
  if(diskbytes(fs, off, fs->inodebuf, fs->inode_size) < 0) return -1;
  put16(fs->inodebuf, ip->mode);
  put32(fs->inodebuf + 4, (uint32)ip->size);
  put16(fs->inodebuf + 26, ip->links);
  put32(fs->inodebuf + 28, ip->sectors);
  put32(fs->inodebuf + 32, ip->flags);
  for(int i = 0; i < 15; i++) put32(fs->inodebuf + 40 + i*4, ip->block[i]);
  if(fs->inode_size >= 112 && (ip->mode & EXT2_S_IFMT) == EXT2_S_IFREG)
    put32(fs->inodebuf + 108, ip->size >> 32);
  return diskbytes_write(fs, off, fs->inodebuf, fs->inode_size);
}

static int
clearinode(struct ext2_fs *fs, uint32 ino)
{
  uchar gd[64];
  uint32 group = (ino - 1) / fs->inodes_per_group;
  uint32 index = (ino - 1) % fs->inodes_per_group;
  if(ino == 0 || ino > fs->inodes || readgd(fs, group, gd) < 0) return -1;
  uint32 table = le32(gd + 8);
  uint64 off = (uint64)table * fs->block_size + (uint64)index * fs->inode_size;
  memset(fs->inodebuf, 0, fs->inode_size);
  return diskbytes_write(fs, off, fs->inodebuf, fs->inode_size);
}

static uint64
max_file_blocks(struct ext2_fs *fs)
{
  uint64 n = fs->block_size / 4;
  return 12 + n + n*n + n*n*n;
}

static uint32
fileblock(struct ext2_fs *fs, struct einode *ip, uint64 logical)
{
  uint64 n = fs->block_size / 4;
  if(logical < 12) return ip->block[logical];
  logical -= 12;
  if(logical < n){
    if(ip->block[12] == 0 || readblock(fs, ip->block[12], fs->ptrbuf[0]) < 0)
      return 0;
    return fs->ptrbuf[0][logical];
  }
  logical -= n;
  if(logical < n*n){
    if(ip->block[13] == 0 || readblock(fs, ip->block[13], fs->ptrbuf[0]) < 0)
      return 0;
    uint32 child = fs->ptrbuf[0][logical / n];
    if(child == 0 || readblock(fs, child, fs->ptrbuf[1]) < 0)
      return 0;
    return fs->ptrbuf[1][logical % n];
  }
  logical -= n*n;
  if(logical >= n*n*n || ip->block[14] == 0 ||
     readblock(fs, ip->block[14], fs->ptrbuf[0]) < 0)
    return 0;
  uint64 per_top = n*n;
  uint32 middle = fs->ptrbuf[0][logical / per_top];
  if(middle == 0 || readblock(fs, middle, fs->ptrbuf[1]) < 0)
    return 0;
  logical %= per_top;
  uint32 leaf = fs->ptrbuf[1][logical / n];
  if(leaf == 0 || readblock(fs, leaf, fs->ptrbuf[2]) < 0)
    return 0;
  return fs->ptrbuf[2][logical % n];
}

static int
adjust_super_count(struct ext2_fs *fs, uint32 offset, int delta)
{
  uchar v[4];
  if(diskbytes(fs, 1024 + offset, v, sizeof(v)) < 0) return -1;
  put32(v, le32(v) + delta);
  return diskbytes_write(fs, 1024 + offset, v, sizeof(v));
}

static int
mark_write_session_dirty(struct ext2_fs *fs)
{
  uchar state[2];
  if(fs->dirty) return 0;
  if(diskbytes(fs, 1024 + 58, state, sizeof(state)) < 0) return -1;
  put16(state, 0); // clear EXT2_VALID_FS until a future clean-unmount exists
  fs->direct = 1;   // outside any transaction: stays set even if one aborts
  int r = diskbytes_write(fs, 1024 + 58, state, sizeof(state));
  fs->direct = 0;
  if(r < 0 || sdflush() < 0) return -1;
  fs->dirty = 1;
  printf("ext2: write session active; run e2fsck after unclean power loss\n");
  return 0;
}

// A block freed earlier in the running transaction must not be reused (and
// overwritten in place) before the free commits: an abort or a crash would
// hand it back to its old owner with foreign contents.
static int
freed_in_tx(struct ext2_fs *fs, uint32 bno)
{
  for(uint32 i = 0; i < fs->nfreed; i++)
    if(fs->freed[i] == bno) return 1;
  return 0;
}

static int
alloc_block(struct ext2_fs *fs, uint32 preferred_group, uint32 *out)
{
  uchar gd[64];
  for(uint32 pass = 0; pass < fs->groups; pass++){
    uint32 group = (preferred_group + pass) % fs->groups;
    uint32 first = fs->first_data_block + group * fs->blocks_per_group;
    uint32 count = fs->blocks_per_group;
    if(first >= fs->blocks) continue;
    if(count > fs->blocks - first) count = fs->blocks - first;
    if(readgd(fs, group, gd) < 0 || le16(gd + 12) == 0) continue;
    uint32 bitmap = le32(gd);
    if(readblock(fs, bitmap, fs->blockbuf) < 0) return -1;
    for(uint32 bit = 0; bit < count; bit++){
      if((fs->blockbuf[bit >> 3] & (1U << (bit & 7))) != 0) continue;
      if(freed_in_tx(fs, first + bit)) continue;   // still owned if we abort
      fs->blockbuf[bit >> 3] |= 1U << (bit & 7);
      if(writeblock(fs, bitmap, fs->blockbuf) < 0) return -1;
      put16(gd + 12, le16(gd + 12) - 1);
      if(writegd(fs, group, gd) < 0 || adjust_super_count(fs, 12, -1) < 0)
        return -1;
      *out = first + bit;
      // The block is unreachable until this transaction commits, so it can
      // be zeroed in place instead of through the journal.
      memset(fs->blockbuf, 0, fs->block_size);
      if(writeblock_data(fs, *out, fs->blockbuf) < 0) return -1;
      return 0;
    }
  }
  return -1;
}

static int
free_block(struct ext2_fs *fs, uint32 bno)
{
  uchar gd[64];
  if(bno < fs->first_data_block || bno >= fs->blocks) return -1;
  uint32 group = (bno - fs->first_data_block) / fs->blocks_per_group;
  uint32 bit = (bno - fs->first_data_block) % fs->blocks_per_group;
  if(readgd(fs, group, gd) < 0) return -1;
  uint32 bitmap = le32(gd);
  if(readblock(fs, bitmap, fs->blockbuf) < 0 ||
     (fs->blockbuf[bit >> 3] & (1U << (bit & 7))) == 0) return -1;
  fs->blockbuf[bit >> 3] &= ~(1U << (bit & 7));
  if(writeblock(fs, bitmap, fs->blockbuf) < 0) return -1;
  put16(gd + 12, le16(gd + 12) + 1);
  if(fs->nfreed < E2_MAXFREED) fs->freed[fs->nfreed++] = bno;
  return writegd(fs, group, gd) < 0 ? -1 : adjust_super_count(fs, 12, 1);
}

static int
alloc_inode(struct ext2_fs *fs, uint32 preferred_group, uint16 mode, struct einode *ip)
{
  uchar gd[64];
  for(uint32 pass = 0; pass < fs->groups; pass++){
    uint32 group = (preferred_group + pass) % fs->groups;
    uint32 count = fs->inodes_per_group;
    uint32 first = group * fs->inodes_per_group + 1;
    if(first > fs->inodes) continue;
    if(count > fs->inodes - first + 1) count = fs->inodes - first + 1;
    if(readgd(fs, group, gd) < 0 || le16(gd + 14) == 0) continue;
    uint32 bitmap = le32(gd + 4);
    if(readblock(fs, bitmap, fs->blockbuf) < 0) return -1;
    for(uint32 bit = 0; bit < count; bit++){
      uint32 ino = first + bit;
      if(ino < fs->first_ino ||
         (fs->blockbuf[bit >> 3] & (1U << (bit & 7))) != 0) continue;
      fs->blockbuf[bit >> 3] |= 1U << (bit & 7);
      if(writeblock(fs, bitmap, fs->blockbuf) < 0) return -1;
      put16(gd + 14, le16(gd + 14) - 1);
      if((mode & EXT2_S_IFMT) == EXT2_S_IFDIR)
        put16(gd + 16, le16(gd + 16) + 1);
      if(writegd(fs, group, gd) < 0 || adjust_super_count(fs, 16, -1) < 0)
        return -1;
      if(clearinode(fs, ino) < 0) return -1;
      memset(ip, 0, sizeof(*ip));
      ip->ino = ino;
      ip->mode = mode;
      ip->links = 1;
      if(writeinode(fs, ip) < 0) return -1;
      return 0;
    }
  }
  return -1;
}

static int
free_inode(struct ext2_fs *fs, uint32 ino, int directory)
{
  uchar gd[64];
  if(ino < fs->first_ino || ino > fs->inodes) return -1;
  uint32 group = (ino - 1) / fs->inodes_per_group;
  uint32 bit = (ino - 1) % fs->inodes_per_group;
  if(readgd(fs, group, gd) < 0) return -1;
  uint32 bitmap = le32(gd + 4);
  if(readblock(fs, bitmap, fs->blockbuf) < 0 ||
     (fs->blockbuf[bit >> 3] & (1U << (bit & 7))) == 0) return -1;
  fs->blockbuf[bit >> 3] &= ~(1U << (bit & 7));
  if(writeblock(fs, bitmap, fs->blockbuf) < 0) return -1;
  put16(gd + 14, le16(gd + 14) + 1);
  if(directory){
    uint16 used = le16(gd + 16);
    if(used == 0) return -1;
    put16(gd + 16, used - 1);
  }
  if(writegd(fs, group, gd) < 0 || adjust_super_count(fs, 16, 1) < 0)
    return -1;
  return clearinode(fs, ino);
}

static int
alloc_pointer_block(struct ext2_fs *fs, struct einode *ip, uint32 group, uint32 *slot)
{
  if(alloc_block(fs, group, slot) < 0)
    return -1;
  ip->sectors += fs->block_size / SECTOR;
  return 0;
}

static int
set_fileblock(struct ext2_fs *fs, struct einode *ip, uint64 logical, uint32 bno)
{
  uint64 n = fs->block_size / 4;
  uint32 group = (ip->ino - 1) / fs->inodes_per_group;
  if(logical < 12){
    ip->block[logical] = bno;
    return 0;
  }
  logical -= 12;
  if(logical < n){
    if(ip->block[12] == 0){
      if(bno == 0) return 0;
      if(alloc_pointer_block(fs, ip, group, &ip->block[12]) < 0) return -1;
    }
    if(readblock(fs, ip->block[12], fs->ptrbuf[0]) < 0) return -1;
    fs->ptrbuf[0][logical] = bno;
    return writeblock(fs, ip->block[12], fs->ptrbuf[0]);
  }
  logical -= n;
  if(logical < n*n){
    uint32 top = logical / n, leaf = logical % n;
    if(ip->block[13] == 0){
      if(bno == 0) return 0;
      if(alloc_pointer_block(fs, ip, group, &ip->block[13]) < 0) return -1;
    }
    if(readblock(fs, ip->block[13], fs->ptrbuf[0]) < 0) return -1;
    if(fs->ptrbuf[0][top] == 0){
      if(bno == 0) return 0;
      if(alloc_pointer_block(fs, ip, group, &fs->ptrbuf[0][top]) < 0 ||
         writeblock(fs, ip->block[13], fs->ptrbuf[0]) < 0) return -1;
    }
    if(readblock(fs, fs->ptrbuf[0][top], fs->ptrbuf[1]) < 0) return -1;
    fs->ptrbuf[1][leaf] = bno;
    return writeblock(fs, fs->ptrbuf[0][top], fs->ptrbuf[1]);
  }
  logical -= n*n;
  if(logical >= n*n*n) return -1;
  uint32 top = logical / (n*n);
  uint32 middle = (logical / n) % n;
  uint32 leaf = logical % n;
  if(ip->block[14] == 0){
    if(bno == 0) return 0;
    if(alloc_pointer_block(fs, ip, group, &ip->block[14]) < 0) return -1;
  }
  if(readblock(fs, ip->block[14], fs->ptrbuf[0]) < 0) return -1;
  if(fs->ptrbuf[0][top] == 0){
    if(bno == 0) return 0;
    if(alloc_pointer_block(fs, ip, group, &fs->ptrbuf[0][top]) < 0 ||
       writeblock(fs, ip->block[14], fs->ptrbuf[0]) < 0) return -1;
  }
  if(readblock(fs, fs->ptrbuf[0][top], fs->ptrbuf[1]) < 0) return -1;
  if(fs->ptrbuf[1][middle] == 0){
    if(bno == 0) return 0;
    if(alloc_pointer_block(fs, ip, group, &fs->ptrbuf[1][middle]) < 0 ||
       writeblock(fs, fs->ptrbuf[0][top], fs->ptrbuf[1]) < 0) return -1;
  }
  if(readblock(fs, fs->ptrbuf[1][middle], fs->ptrbuf[2]) < 0) return -1;
  fs->ptrbuf[2][leaf] = bno;
  return writeblock(fs, fs->ptrbuf[1][middle], fs->ptrbuf[2]);
}

static uint32
get_or_alloc_fileblock(struct ext2_fs *fs, struct einode *ip, uint64 logical)
{
  uint32 bno = fileblock(fs, ip, logical);
  if(bno) return bno;
  uint32 group = (ip->ino - 1) / fs->inodes_per_group;
  if(alloc_block(fs, group, &bno) < 0 || set_fileblock(fs, ip, logical, bno) < 0)
    return 0;
  ip->sectors += fs->block_size / SECTOR;
  return bno;
}

static int
ptr_empty(struct ext2_fs *fs, uint32 *p)
{
  for(uint32 i = 0; i < fs->block_size / 4; i++)
    if(p[i]) return 0;
  return 1;
}

static void
account_free(struct ext2_fs *fs, struct einode *ip)
{
  uint32 sectors = fs->block_size / SECTOR;
  ip->sectors = ip->sectors >= sectors ? ip->sectors - sectors : 0;
}

// Free one data block and prune now-empty indirect blocks on its path.
static int
clear_fileblock(struct ext2_fs *fs, struct einode *ip, uint64 logical)
{
  uint64 n = fs->block_size / 4;
  if(logical < 12){
    if(ip->block[logical] && free_block(fs, ip->block[logical]) < 0) return -1;
    if(ip->block[logical]) account_free(fs, ip);
    ip->block[logical] = 0;
    return 0;
  }
  logical -= 12;
  if(logical < n){
    uint32 root = ip->block[12];
    if(root == 0) return 0;
    if(readblock(fs, root, fs->ptrbuf[0]) < 0) return -1;
    if(fs->ptrbuf[0][logical]){
      if(free_block(fs, fs->ptrbuf[0][logical]) < 0) return -1;
      account_free(fs, ip); fs->ptrbuf[0][logical] = 0;
    }
    if(ptr_empty(fs, fs->ptrbuf[0])){
      if(free_block(fs, root) < 0) return -1;
      account_free(fs, ip); ip->block[12] = 0;
    } else if(writeblock(fs, root, fs->ptrbuf[0]) < 0) return -1;
    return 0;
  }
  logical -= n;
  if(logical < n*n){
    uint32 top = logical / n, leaf = logical % n;
    uint32 root = ip->block[13];
    if(root == 0 || readblock(fs, root, fs->ptrbuf[0]) < 0) return root ? -1 : 0;
    uint32 child = fs->ptrbuf[0][top];
    if(child == 0 || readblock(fs, child, fs->ptrbuf[1]) < 0) return child ? -1 : 0;
    if(fs->ptrbuf[1][leaf]){
      if(free_block(fs, fs->ptrbuf[1][leaf]) < 0) return -1;
      account_free(fs, ip); fs->ptrbuf[1][leaf] = 0;
    }
    if(ptr_empty(fs, fs->ptrbuf[1])){
      if(free_block(fs, child) < 0) return -1;
      account_free(fs, ip); fs->ptrbuf[0][top] = 0;
    } else if(writeblock(fs, child, fs->ptrbuf[1]) < 0) return -1;
    if(ptr_empty(fs, fs->ptrbuf[0])){
      if(free_block(fs, root) < 0) return -1;
      account_free(fs, ip); ip->block[13] = 0;
    } else if(writeblock(fs, root, fs->ptrbuf[0]) < 0) return -1;
    return 0;
  }
  logical -= n*n;
  if(logical >= n*n*n) return -1;
  uint32 top = logical / (n*n), middle = (logical / n) % n, leaf = logical % n;
  uint32 root = ip->block[14];
  if(root == 0 || readblock(fs, root, fs->ptrbuf[0]) < 0) return root ? -1 : 0;
  uint32 midblock = fs->ptrbuf[0][top];
  if(midblock == 0 || readblock(fs, midblock, fs->ptrbuf[1]) < 0)
    return midblock ? -1 : 0;
  uint32 leafblock = fs->ptrbuf[1][middle];
  if(leafblock == 0 || readblock(fs, leafblock, fs->ptrbuf[2]) < 0)
    return leafblock ? -1 : 0;
  if(fs->ptrbuf[2][leaf]){
    if(free_block(fs, fs->ptrbuf[2][leaf]) < 0) return -1;
    account_free(fs, ip); fs->ptrbuf[2][leaf] = 0;
  }
  if(ptr_empty(fs, fs->ptrbuf[2])){
    if(free_block(fs, leafblock) < 0) return -1;
    account_free(fs, ip); fs->ptrbuf[1][middle] = 0;
  } else if(writeblock(fs, leafblock, fs->ptrbuf[2]) < 0) return -1;
  if(ptr_empty(fs, fs->ptrbuf[1])){
    if(free_block(fs, midblock) < 0) return -1;
    account_free(fs, ip); fs->ptrbuf[0][top] = 0;
  } else if(writeblock(fs, midblock, fs->ptrbuf[1]) < 0) return -1;
  if(ptr_empty(fs, fs->ptrbuf[0])){
    if(free_block(fs, root) < 0) return -1;
    account_free(fs, ip); ip->block[14] = 0;
  } else if(writeblock(fs, root, fs->ptrbuf[0]) < 0) return -1;
  return 0;
}

static int tx_checkpoint(struct ext2_fs *fs, struct einode *ip);

static int
truncate_inode(struct ext2_fs *fs, struct einode *ip, uint64 newsize)
{
  uint64 maxsize = max_file_blocks(fs) * fs->block_size;
  if(newsize > maxsize) return -1;
  if(newsize < ip->size){
    uint64 oldblocks = (ip->size + fs->block_size - 1) / fs->block_size;
    uint64 keepblocks = (newsize + fs->block_size - 1) / fs->block_size;
    while(oldblocks > keepblocks){
      if(clear_fileblock(fs, ip, --oldblocks) < 0) return -1;
      // Keep the inode consistent with the blocks freed so far, so a split
      // transaction (tx_checkpoint) never commits dangling pointers.
      if(ip->size > oldblocks * fs->block_size)
        ip->size = oldblocks * fs->block_size;
      if(fs->journal && xj_space() <= 96 && tx_checkpoint(fs, ip) < 0) return -1;
    }
    if(newsize && (newsize % fs->block_size)){
      uint32 bno = fileblock(fs, ip, newsize / fs->block_size);
      if(bno){
        if(readblock(fs, bno, fs->blockbuf) < 0) return -1;
        memset(fs->blockbuf + newsize % fs->block_size, 0,
               fs->block_size - newsize % fs->block_size);
        if(writeblock_data(fs, bno, fs->blockbuf) < 0) return -1;
      }
    }
  }
  ip->size = newsize;
  return writeinode(fs, ip);
}

static int
nameeq(const uchar *p, int n, const char *s, int sn)
{
  if(n != sn) return 0;
  for(int i = 0; i < n; i++) if(p[i] != (uchar)s[i]) return 0;
  return 1;
}

static int
e2dirlookup(struct ext2_fs *fs, struct einode *dp, const char *name, int namelen, uint32 *ino)
{
  uint64 pos = 0, loaded = (uint64)-1;
  if((dp->mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -1;
  while(pos + 8 <= dp->size){
    uint64 logical = pos / fs->block_size;
    uint32 inblock = pos % fs->block_size;
    if(logical != loaded){
      uint32 b = fileblock(fs, dp, logical);
      if(b == 0 || readblock(fs, b, fs->blockbuf) < 0) return -1;
      loaded = logical;
    }
    if(inblock + 8 > fs->block_size) return -1;
    uchar *de = fs->blockbuf + inblock;
    uint32 deino = le32(de);
    uint16 reclen = le16(de + 4);
    uint8 nlen = de[6];
    if(reclen < 8 || (reclen & 3) || inblock + reclen > fs->block_size)
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
e2diradd(struct ext2_fs *fs, struct einode *dp, const char *name, int namelen, uint32 ino, uint8 type)
{
  uint32 need = (8 + namelen + 3) & ~3U;
  uint64 nblock = (dp->size + fs->block_size - 1) / fs->block_size;
  for(uint64 logical = 0; logical < nblock; logical++){
    uint32 bno = fileblock(fs, dp, logical);
    if(bno == 0 || readblock(fs, bno, fs->blockbuf) < 0) return -1;
    for(uint32 pos = 0; pos + 8 <= fs->block_size; ){
      uchar *de = fs->blockbuf + pos;
      uint32 deino = le32(de);
      uint16 reclen = le16(de + 4);
      uint8 nlen = de[6];
      if(reclen < 8 || (reclen & 3) || pos + reclen > fs->block_size)
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
        de = fs->blockbuf + slot;
        memset(de, 0, slotlen);
        put32(de, ino);
        put16(de + 4, slotlen);
        de[6] = namelen;
        de[7] = type;
        memmove(de + 8, name, namelen);
        return writeblock(fs, bno, fs->blockbuf);
      }
      pos += reclen;
    }
  }

  if(nblock >= max_file_blocks(fs)) return -1;
  uint32 bno = get_or_alloc_fileblock(fs, dp, nblock);
  if(bno == 0) return -1;
  memset(fs->blockbuf, 0, fs->block_size);
  put32(fs->blockbuf, ino);
  put16(fs->blockbuf + 4, fs->block_size);
  fs->blockbuf[6] = namelen;
  fs->blockbuf[7] = type;
  memmove(fs->blockbuf + 8, name, namelen);
  if(writeblock(fs, bno, fs->blockbuf) < 0) return -1;
  dp->size = (uint64)(nblock + 1) * fs->block_size;
  return writeinode(fs, dp);
}

static int
e2dirremove(struct ext2_fs *fs, struct einode *dp, const char *name, int namelen)
{
  uint64 pos = 0, loaded = (uint64)-1;
  uint32 bno = 0;
  while(pos + 8 <= dp->size){
    uint64 logical = pos / fs->block_size;
    uint32 inblock = pos % fs->block_size;
    if(logical != loaded){
      bno = fileblock(fs, dp, logical);
      if(bno == 0 || readblock(fs, bno, fs->blockbuf) < 0) return -1;
      loaded = logical;
    }
    uchar *de = fs->blockbuf + inblock;
    uint32 deino = le32(de);
    uint16 reclen = le16(de + 4);
    uint8 nlen = de[6];
    if(reclen < 8 || (reclen & 3) || inblock + reclen > fs->block_size)
      return -1;
    if(deino && nlen <= reclen - 8 && nameeq(de + 8, nlen, name, namelen)){
      put32(de, 0);
      return writeblock(fs, bno, fs->blockbuf);
    }
    pos += reclen;
  }
  return -1;
}

static int
e2dirempty(struct ext2_fs *fs, struct einode *dp)
{
  uint64 pos = 0, loaded = (uint64)-1;
  while(pos + 8 <= dp->size){
    uint64 logical = pos / fs->block_size;
    uint32 inblock = pos % fs->block_size;
    if(logical != loaded){
      uint32 bno = fileblock(fs, dp, logical);
      if(bno == 0 || readblock(fs, bno, fs->blockbuf) < 0) return 0;
      loaded = logical;
    }
    uchar *de = fs->blockbuf + inblock;
    uint32 deino = le32(de);
    uint16 reclen = le16(de + 4);
    uint8 nlen = de[6];
    if(reclen < 8 || (reclen & 3) || inblock + reclen > fs->block_size)
      return 0;
    if(deino && !(nlen == 1 && de[8] == '.') &&
       !(nlen == 2 && de[8] == '.' && de[9] == '.'))
      return 0;
    pos += reclen;
  }
  return 1;
}

static int
e2dirreplace(struct ext2_fs *fs, struct einode *dp, const char *name, int namelen, uint32 ino)
{
  uint64 pos = 0, loaded = (uint64)-1;
  uint32 bno = 0;
  while(pos + 8 <= dp->size){
    uint64 logical = pos / fs->block_size;
    uint32 inblock = pos % fs->block_size;
    if(logical != loaded){
      bno = fileblock(fs, dp, logical);
      if(bno == 0 || readblock(fs, bno, fs->blockbuf) < 0) return -1;
      loaded = logical;
    }
    uchar *de = fs->blockbuf + inblock;
    uint16 reclen = le16(de + 4);
    uint8 nlen = de[6];
    if(reclen < 8 || (reclen & 3) || inblock + reclen > fs->block_size)
      return -1;
    if(le32(de) && nlen <= reclen - 8 && nameeq(de + 8, nlen, name, namelen)){
      put32(de, ino);
      return writeblock(fs, bno, fs->blockbuf);
    }
    pos += reclen;
  }
  return -1;
}

static int
lookup(struct ext2_fs *fs, const char *path, uint32 *ino, struct einode *ip)
{
  const char *p = path, *start;
  uint32 curino = EXT2_ROOT_INO, nextino;
  struct einode cur;
  if(readinode(fs, curino, &cur) < 0 || inode_supported(&cur) < 0)
    return -1;
  while(*p == '/') p++;
  while(*p){
    start = p;
    while(*p && *p != '/') p++;
    if(e2dirlookup(fs, &cur, start, p - start, &nextino) < 0) return -1;
    curino = nextino;
    if(readinode(fs, curino, &cur) < 0 || inode_supported(&cur) < 0)
      return -1;
    while(*p == '/') p++;
  }
  *ino = curino;
  *ip = cur;
  return 0;
}

static int
lookupparent(struct ext2_fs *fs, const char *path, struct einode *parent, char *name, int *namelen)
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
  if(lookup(fs, buf, &pino, parent) < 0 ||
     (parent->mode & EXT2_S_IFMT) != EXT2_S_IFDIR)
    return -1;
  return 0;
}

static int
setup_ext2(struct ext2_fs *fs, uint32 start, uint32 sectors, int partno)
{
  uchar sb[1024];
  uint32 compat, incompat, rocompat;
  fs->part_lba = start;
  fs->part_sectors = sectors;
  if(diskbytes(fs, 1024, sb, sizeof(sb)) < 0 || le16(sb + 56) != EXT2_MAGIC){
    printf("ext2: p%d has no ext superblock\n", partno); return -1;
  }
  compat = le32(sb + 92);
  incompat = le32(sb + 96);
  rocompat = le32(sb + 100);
  if(compat & EXT2_FEATURE_COMPAT_HAS_JOURNAL){
    printf("ext2: p%d has a journal; use mke2fs -t ext2\n", partno);
    return -1;
  }
  uint32 compat_ok = EXT2_FEATURE_COMPAT_EXT_ATTR |
                     EXT2_FEATURE_COMPAT_RESIZE_INO |
                     EXT2_FEATURE_COMPAT_DIR_INDEX;
  if(compat & ~compat_ok){
    printf("ext2: p%d unsupported compat features=0x%x\n", partno, compat);
    return -1;
  }
  // This driver always emits ext2_dir_entry_2 records, so FILETYPE is not
  // merely optional here: accepting the old 16-bit name_len format and then
  // writing byte 7 as a type would corrupt a directory.
  if((incompat & EXT2_FEATURE_INCOMPAT_FILETYPE) == 0 ||
     (incompat & ~EXT2_FEATURE_INCOMPAT_FILETYPE)){
    printf("ext2: p%d unsupported incompat features=0x%x\n", partno, incompat);
    return -1;
  }
  uint32 rocompat_ok = EXT2_FEATURE_RO_SPARSE_SUPER |
                       EXT2_FEATURE_RO_LARGE_FILE;
  if(rocompat & ~rocompat_ok){
    printf("ext2: p%d unsupported ro-compat features=0x%x\n", partno, rocompat);
    return -1;
  }
  fs->block_size = 1024U << le32(sb + 24);
  if(fs->block_size < 1024 || fs->block_size > BLK_MAX_BSIZE){
    printf("ext2: p%d unsupported block size %d\n", partno, fs->block_size); return -1;
  }
  fs->inodes = le32(sb);
  fs->blocks = le32(sb + 4);
  fs->first_data_block = le32(sb + 20);
  fs->first_ino = le32(sb + 76) ? le32(sb + 84) : 11;
  if(fs->blocks == 0 || fs->inodes == 0 ||
     (uint64)fs->blocks * fs->block_size > (uint64)sectors * SECTOR){
    printf("ext2: p%d geometry exceeds partition\n", partno);
    return -1;
  }
  fs->blocks_per_group = le32(sb + 32);
  fs->inodes_per_group = le32(sb + 40);
  fs->inode_size = le32(sb + 76) ? le16(sb + 88) : 128;
  if(fs->inode_size < 128 || fs->inode_size > sizeof(fs->inodebuf) ||
     fs->inodes_per_group == 0 || fs->blocks_per_group == 0 ||
     fs->inodes_per_group > fs->block_size * 8 ||
     fs->blocks_per_group > fs->block_size * 8)
    return -1;
  fs->desc_size = 32;
  fs->groups = (fs->blocks - fs->first_data_block +
               fs->blocks_per_group - 1) / fs->blocks_per_group;
  fs->gd_block = fs->block_size == 1024 ? 2 : 1;
  // setup_ext2() read the fixed 1024-byte superblock using the partition's
  // initial 512-byte cache geometry.  Drop those entries before changing the
  // cache key to the filesystem block size.
  if(binvalidate(fs->dev) < 0 || blkdev_set_bsize(fs->dev, fs->block_size) < 0)
    return -1;
  fs->ready = 1;
  fs->dirty = 0;
  fs->was_unclean = (le16(sb + 58) & 1U) == 0;
  if(fs->was_unclean)
    printf("ext2: p%d was not clean; preserving dirty state until e2fsck\n",
           partno);
  printf("ext2: p%d ready rw start=%d sectors=%d block=%d groups=%d\n",
         partno, start, sectors, fs->block_size, fs->groups);
  return 0;
}

// External journal: the unused tail of the raw xv6 root partition.  The
// invariant ext2 geometry is read first so the home partition has its final
// cache block size; replay then completes before VFS exposes the mount or any
// mutable metadata is used.
static int
ext2_journal_init(struct ext2_fs *fs)
{
  // There is one external log region.  Only one mounted ext2 instance may
  // own it; additional instances remain valid ext2 mounts but write through
  // without this optional journal.
  if(xj_owner_dev && xj_owner_dev != fs->dev)
    return 0;
  struct blkdev *jbd = rootdev_xv6_part();
  struct blkdev *home = blkdev_get(fs->dev);
  uint32 jstart = FSSIZE * (BSIZE / SECTOR);
  if(jbd && jbd->disk && jbd->nsect > jstart && home){
    if(xj_init(jbd, jstart, jbd->nsect - jstart, home) < 0)
      return -1;
    fs->journal = 1;
    xj_owner_dev = fs->dev;
  } else {
    printf("ext2: no raw xv6 root partition; external journal disabled\n");
  }
  return 0;
}

static void
ext2_drop_instance(struct ext2_fs *fs)
{
  if(fs->blockbuf){
    kfree(fs->blockbuf);
    fs->blockbuf = 0;
  }
  for(int i = 0; i < 3; i++)
    if(fs->ptrbuf[i]){
      kfree(fs->ptrbuf[i]);
      fs->ptrbuf[i] = 0;
    }
  memset(fs->open, 0, sizeof(fs->open));
  memset(fs->freed, 0, sizeof(fs->freed));
  fs->ready = 0;
  fs->dirty = 0;
  fs->was_unclean = 0;
  fs->journal = 0;
  fs->direct = 0;
  fs->nfreed = 0;
  fs->dev = 0;
  fs->part_lba = 0;
  fs->part_sectors = 0;
  fs->blocks = fs->inodes = fs->groups = 0;
}

// fill_super (Linux ext2_fill_super): open the ext2 on partition dev, whose
// place on the card comes from the block-device table, not from the MBR.
// After reading invariant geometry, the journal is recovered before the
// mounted filesystem is published.  Each block device owns a separate
// ext2_fs through super_block.priv.
static int
ext2_fill_super(int dev, int flags)
{
  struct blkdev *bd = blkdev_get(dev);
  struct ext2_fs *fs = ext2_by_dev(dev);
  int r = -1;
  if(bd == 0 || bd->disk == 0 || fs == 0)
    return -1;
  acquiresleep(&ext2_mount_lock);
  acquiresleep(&fs->lock);
  int was_ready = fs->ready;
  if(was_ready && fs->dev == dev)
    r = 0;
  if(!fs->ready){
    fs->dev = dev;
    fs->part_lba = bd->start;
    fs->part_sectors = bd->nsect;
    fs->block_size = blkdev_bsize(dev);
    if(fs->blockbuf == 0)
      fs->blockbuf = (uchar*)kalloc();
    for(int i = 0; i < 3; i++)
      if(fs->ptrbuf[i] == 0)
        fs->ptrbuf[i] = (uint32*)kalloc();
    if(fs->blockbuf && fs->ptrbuf[0] && fs->ptrbuf[1] && fs->ptrbuf[2]){
      if(setup_ext2(fs, bd->start, bd->nsect, bd->partno) == 0 &&
         ext2_journal_init(fs) == 0){
        struct einode root;
        if(readinode(fs, EXT2_ROOT_INO, &root) == 0 &&
           inode_supported(&root) == 0 &&
           (root.mode & EXT2_S_IFMT) == EXT2_S_IFDIR)
          r = 0;
        else
          printf("ext2: invalid or unsupported root inode\n");
      }
    }
  }
  if(!was_ready && r < 0){
    if(fs->journal){
      xj_shutdown(bd);
      xj_owner_dev = 0;
    }
    binvalidate(dev);
    blkdev_set_bsize(dev, SECTOR);
    ext2_drop_instance(fs);
  }
  releasesleep(&fs->lock);
  releasesleep(&ext2_mount_lock);
  if(r < 0)
    return -1;
  return ext2_register_super(dev, (flags & VFS_MOUNT_RDONLY) != 0);
}

// source names a device node ("/dev/root"), or, as in /etc/fstab, it is just
// "ext2": then the partition already set up, else the first Linux (0x83)
// partition that holds an ext2.
static int
ext2_get_sb(struct fs_type *t, char *source, int flags)
{
  int dev = lookup_bdev(source);
  struct ext2_fs *fs = ext2_default();
  if(dev < 0 && fs)
    dev = fs->dev;
  for(int i = 0; dev < 0 && i < NBLKDEV; i++){
    struct blkdev *bd = blkdev_at(i);
    if(bd && bd->disk && bd->ptype == 0x83 && t->fill_super(bd->dev, flags) == 0)
      dev = bd->dev;
  }
  if(dev <= 0 || dev >= NSUPER || blkdev_get(dev) == 0)
    return -1;
  if(getsuper(dev) == 0 && t->fill_super(dev, flags) < 0)
    return -1;
  return dev;
}

static struct fs_type ext2_type = {
  .name = "ext2",
  .get_sb = ext2_get_sb,
  .fill_super = ext2_fill_super,
};

// Linux init_ext2_fs(): only make the type known; the partition is opened
// when it is mounted.
void
ext2init(void)
{
  initsleeplock(&ext2_mount_lock, "ext2-mount");
  for(int dev = 0; dev < NSUPER; dev++){
    memset(&ext2_instances[dev], 0, sizeof(ext2_instances[dev]));
    initsleeplock(&ext2_instances[dev].lock, "ext2");
  }
  xj_owner_dev = 0;
  register_filesystem(&ext2_type);
}

// Order 5: registers the type before vfsinit() (order 6) lists them.
late_initcall_ord("ext2", ext2init, 5);

int
ext2ready(void)
{
  return ext2_default() != 0;
}

// First card sector of the partition the driver mounted (0 if none), so
// do_mounts.c can check that /dev/root names the same partition.
uint32
ext2_part_lba(void)
{
  struct ext2_fs *fs = ext2_default();
  return fs ? fs->part_lba : 0;
}

// ---------------------------------------------------------------------------
// Transactions.  With the external journal every mutating operation is one
// xjournal transaction: metadata sectors are captured, data sectors are
// written directly first (ordered mode), and a failed operation is aborted so
// nothing it changed reaches the disk.  Without the journal (no raw xv6 root
// partition) writes go straight to the card as before.

static void
tx_begin(struct ext2_fs *fs)
{
  fs->nfreed = 0;
  if(fs->journal)
    xj_begin();
}

static int
tx_end(struct ext2_fs *fs, int ok)
{
  fs->nfreed = 0;
  if(!fs->journal)
    return ok ? 0 : -1;
  if(!ok){
    xj_abort();
    return -1;
  }
  int r = xj_commit();
  return r;
}

static int
mark_write_session_clean(struct ext2_fs *fs)
{
  uchar state[2];
  if(!fs->dirty)
    return sdflush();
  // A clean shutdown can close only the write session started by this mount.
  // If the volume arrived dirty, do not falsely certify older unchecked
  // metadata as consistent merely because this session is ending cleanly.
  if(fs->was_unclean){
    fs->dirty = 0;
    return sdflush();
  }
  if(diskbytes(fs, 1024 + 58, state, sizeof(state)) < 0)
    return -1;
  put16(state, 1);               // EXT2_VALID_FS
  tx_begin(fs);
  int ok = diskbytes_write(fs, 1024 + 58, state, sizeof(state)) == 0;
  if(tx_end(fs, ok) < 0 || sdflush() < 0)
    return -1;
  fs->dirty = 0;
  return 0;
}

// Very large truncates or writes touch more metadata than one transaction
// holds.  At a consistent point (the inode written with a matching size) the
// work so far is committed and a new transaction continues.
static int
tx_checkpoint(struct ext2_fs *fs, struct einode *ip)
{
  if(!fs->journal || xj_space() > 96)
    return 0;
  if(writeinode(fs, ip) < 0 || xj_commit() < 0)
    return -1;
  fs->nfreed = 0;
  xj_begin();
  return 0;
}

// ---------------------------------------------------------------------------
// Open-inode table.  VFS vnodes for ext2 files are bound to an inode number at
// open time, so I/O on an open descriptor never re-resolves the path.  An
// inode whose last name is removed while open becomes an orphan: its blocks
// are released when the last descriptor closes, as in Unix.

static int
open_slot(struct ext2_fs *fs, uint32 ino)
{
  for(int i = 0; i < NFILE; i++)
    if(fs->open[i].ref > 0 && fs->open[i].ino == ino)
      return i;
  return -1;
}

// Drop one link from an inode whose directory entry is already gone.
static int
drop_link(struct ext2_fs *fs, struct einode *ip)
{
  int directory = (ip->mode & EXT2_S_IFMT) == EXT2_S_IFDIR;
  if(directory)
    ip->links = 0;
  else if(ip->links)
    ip->links--;
  if(ip->links)
    return writeinode(fs, ip);
  int s = open_slot(fs, ip->ino);
  if(s >= 0){
    fs->open[s].orphan = 1;
    return writeinode(fs, ip);       // links=0 on disk, blocks kept until close
  }
  if(truncate_inode(fs, ip, 0) < 0)
    return -1;
  return free_inode(fs, ip->ino, directory);
}

int
ext2open(char *path, uint64 *handle)
{
  struct ext2_fs *fs = ext2_default();
  if(fs == 0) return -1;
  struct einode ip;
  uint32 ino;
  int free = -1;
  acquiresleep(&fs->lock);
  if(lookup(fs, path, &ino, &ip) < 0){
    releasesleep(&fs->lock);
    return -1;
  }
  int s = open_slot(fs, ino);
  if(s < 0){
    for(int i = 0; i < NFILE; i++)
      if(fs->open[i].ref == 0){ free = i; break; }
    if(free < 0){
      releasesleep(&fs->lock);
      return -1;
    }
    s = free;
    fs->open[s].ino = ino;
    fs->open[s].orphan = 0;
  }
  fs->open[s].ref++;
  *handle = ino;
  releasesleep(&fs->lock);
  return 0;
}

void
ext2release(int dev, uint64 handle)
{
  struct ext2_fs *fs = ext2_by_dev(dev);
  if(fs == 0 || !fs->ready) return;
  struct einode ip;
  acquiresleep(&fs->lock);
  int s = open_slot(fs, (uint32)handle);
  if(s < 0)
    panic("ext2release");
  if(--fs->open[s].ref == 0 && fs->open[s].orphan){
    fs->open[s].orphan = 0;
    tx_begin(fs);
    int ok = readinode(fs, (uint32)handle, &ip) == 0 && ip.links == 0 &&
             truncate_inode(fs, &ip, 0) == 0 &&
             free_inode(fs, ip.ino, (ip.mode & EXT2_S_IFMT) == EXT2_S_IFDIR) == 0;
    if(tx_end(fs, ok) < 0)
      printf("ext2: orphan inode %d not reclaimed; run e2fsck\n", (int)handle);
  }
  releasesleep(&fs->lock);
}

// ---------------------------------------------------------------------------
// Reads.

static int
read_locked(struct ext2_fs *fs, struct einode *ip, uint64 off, void *dst, int n)
{
  int done = 0;
  if(n < 0 || (ip->mode & EXT2_S_IFMT) != EXT2_S_IFREG) return -1;
  if(off >= ip->size) return 0;
  if((uint64)n > ip->size - off) n = ip->size - off;
  while(done < n){
    uint64 logical = off / fs->block_size;
    uint32 boff = off % fs->block_size;
    int take = fs->block_size - boff;
    uint32 b = fileblock(fs, ip, logical);
    if(take > n - done) take = n - done;
    if(b == 0) memset(fs->blockbuf, 0, fs->block_size);
    else if(readblock(fs, b, fs->blockbuf) < 0) return -1;
    memmove((uchar*)dst + done, fs->blockbuf + boff, take);
    off += take; done += take;
  }
  return done;
}

int
ext2readfile(char *path, uint64 off, void *dst, int n)
{
  struct ext2_fs *fs = ext2_default();
  if(fs == 0) return -1;
  struct einode ip;
  uint32 ino;
  int r = -1;
  acquiresleep(&fs->lock);
  if(lookup(fs, path, &ino, &ip) == 0)
    r = read_locked(fs, &ip, off, dst, n);
  releasesleep(&fs->lock);
  return r;
}

int
ext2readino(int dev, uint64 handle, uint64 off, void *dst, int n)
{
  struct ext2_fs *fs = ext2_by_dev(dev);
  if(fs == 0 || !fs->ready) return -1;
  struct einode ip;
  int r = -1;
  acquiresleep(&fs->lock);
  if(readinode(fs, (uint32)handle, &ip) == 0)
    r = read_locked(fs, &ip, off, dst, n);
  releasesleep(&fs->lock);
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
  struct ext2_fs *fs = ext2_default();
  if(fs == 0) return -1;
  struct einode ip;
  uint32 inum;
  int r = -1;
  acquiresleep(&fs->lock);
  if(lookup(fs, path, &inum, &ip) == 0){
    fill_stat(&ip, ino, mode, size);
    r = 0;
  }
  releasesleep(&fs->lock);
  return r;
}

int
ext2statino(int dev, uint64 handle, uint *ino, ushort *mode, uint64 *size)
{
  struct ext2_fs *fs = ext2_by_dev(dev);
  if(fs == 0 || !fs->ready) return -1;
  struct einode ip;
  int r = -1;
  acquiresleep(&fs->lock);
  if(readinode(fs, (uint32)handle, &ip) == 0){
    fill_stat(&ip, ino, mode, size);
    r = 0;
  }
  releasesleep(&fs->lock);
  return r;
}

// The index-th entry of directory dp (fs->lock held).  1: found, 0: end.
static int
readdir_core(struct ext2_fs *fs, struct einode *dpp, int index, struct ext2_user_dirent *out)
{
  struct einode dp = *dpp, child;
  uint64 pos = 0, loaded = (uint64)-1;
  int seen = 0;
  if(index < 0 || (dp.mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -1;
  while(pos + 8 <= dp.size){
    uint64 logical = pos / fs->block_size;
    uint32 inblock = pos % fs->block_size;
    if(logical != loaded){
      uint32 b = fileblock(fs, &dp, logical);
      if(b == 0 || readblock(fs, b, fs->blockbuf) < 0) break;
      loaded = logical;
    }
    uchar *de = fs->blockbuf + inblock;
    uint32 deino = le32(de);
    uint16 reclen = le16(de + 4);
    uint8 nlen = de[6];
    uint8 type = de[7];
    if(reclen < 8 || (reclen & 3) || inblock + reclen > fs->block_size) break;
    if(deino && nlen <= reclen - 8){
      if(seen++ == index){
        memset(out, 0, sizeof(*out));
        out->inode = deino; out->type = type;
        memmove(out->name, de + 8, nlen);
        out->name[nlen] = 0;
        if(readinode(fs, deino, &child) == 0){ out->size = child.size; out->mode = child.mode; }
        return 1;
      }
    }
    pos += reclen;
  }
  return 0;
}

int
ext2readdir(char *path, int index, struct ext2_user_dirent *out)
{
  struct ext2_fs *fs = ext2_default();
  if(fs == 0) return -1;
  struct einode dp;
  uint32 ino;
  int r = -1;
  acquiresleep(&fs->lock);
  if(lookup(fs, path, &ino, &dp) == 0)
    r = readdir_core(fs, &dp, index, out);
  releasesleep(&fs->lock);
  return r;
}

// ---------------------------------------------------------------------------
// Mutations.  Each public entry point is lock + transaction + *_locked().

// Create regular file name in directory *parent (fs->lock held, in a
// transaction); the new inode number goes to *out.
static int
create_core(struct ext2_fs *fs, struct einode *parent, const char *name, int namelen, uint32 *out)
{
  struct einode child;
  uint32 ino;
  if(!fs->ready || e2dirlookup(fs, parent, name, namelen, &ino) == 0 ||
     mark_write_session_dirty(fs) < 0)
    return -1;
  uint32 group = (parent->ino - 1) / fs->inodes_per_group;
  if(alloc_inode(fs, group, EXT2_S_IFREG | 0644, &child) < 0 ||
     e2diradd(fs, parent, name, namelen, child.ino, 1) < 0)
    return -1;               // the aborted transaction also undoes alloc_inode
  *out = child.ino;
  return 0;
}

static int
create_locked(struct ext2_fs *fs, char *path)
{
  struct einode parent, existing;
  uint32 ino;
  char name[EXT2_NAME_MAX + 1];
  int namelen;
  if(!fs->ready || lookup(fs, path, &ino, &existing) == 0 ||
     lookupparent(fs, path, &parent, name, &namelen) < 0)
    return -1;
  return create_core(fs, &parent, name, namelen, &ino);
}

int
ext2createfile(char *path)
{
  struct ext2_fs *fs = ext2_default();
  if(fs == 0) return -1;
  acquiresleep(&fs->lock);
  tx_begin(fs);
  int r = tx_end(fs, create_locked(fs, path) == 0);
  releasesleep(&fs->lock);
  return r;
}

static int
truncate_locked(struct ext2_fs *fs, struct einode *ip, uint64 size)
{
  if((ip->mode & EXT2_S_IFMT) != EXT2_S_IFREG ||
     mark_write_session_dirty(fs) < 0)
    return -1;
  return truncate_inode(fs, ip, size);
}

int
ext2truncatefile(char *path, uint64 size)
{
  struct ext2_fs *fs = ext2_default();
  if(fs == 0) return -1;
  struct einode ip;
  uint32 ino;
  acquiresleep(&fs->lock);
  tx_begin(fs);
  int ok = lookup(fs, path, &ino, &ip) == 0 && truncate_locked(fs, &ip, size) == 0;
  int r = tx_end(fs, ok);
  releasesleep(&fs->lock);
  return r;
}

int
ext2truncino(int dev, uint64 handle, uint64 size)
{
  struct ext2_fs *fs = ext2_by_dev(dev);
  if(fs == 0 || !fs->ready) return -1;
  struct einode ip;
  acquiresleep(&fs->lock);
  tx_begin(fs);
  int ok = readinode(fs, (uint32)handle, &ip) == 0 &&
           truncate_locked(fs, &ip, size) == 0;
  int r = tx_end(fs, ok);
  releasesleep(&fs->lock);
  return r;
}

// Returns bytes written; on a partial failure the completed prefix is kept.
static int
write_locked(struct ext2_fs *fs, struct einode *ip, uint64 off, int user_src, uint64 src, int n)
{
  int done = 0;
  uint64 maxsize = max_file_blocks(fs) * fs->block_size;
  if(n < 0 || off + n < off || off + n > maxsize ||
     (ip->mode & EXT2_S_IFMT) != EXT2_S_IFREG ||
     mark_write_session_dirty(fs) < 0)
    return -1;
  while(done < n){
    uint64 logical = off / fs->block_size;
    uint32 boff = off % fs->block_size;
    int take = fs->block_size - boff;
    if(take > n - done) take = n - done;
    if(tx_checkpoint(fs, ip) < 0) break;
    uint32 bno = get_or_alloc_fileblock(fs, ip, logical);
    if(bno == 0 || readblock(fs, bno, fs->blockbuf) < 0 ||
       either_copyin(fs->blockbuf + boff, user_src, src + done, take) < 0 ||
       writeblock_data(fs, bno, fs->blockbuf) < 0)
      break;
    off += take;
    done += take;
    if(off > ip->size) ip->size = off;
  }
  if(done == 0)
    return -1;
  return writeinode(fs, ip) < 0 ? -1 : done;
}

static int
write_common(struct ext2_fs *fs, struct einode *ip, uint64 off, int user_src, uint64 src, int n)
{
  int r = write_locked(fs, ip, off, user_src, src, n);
  if(tx_end(fs, r > 0) < 0)
    return -1;
  return r;
}

int
ext2writefile(char *path, struct fat32_file *ignored, uint64 off,
              int user_src, uint64 src, int n)
{
  struct ext2_fs *fs = ext2_default();
  if(fs == 0) return -1;
  struct einode ip;
  uint32 ino;
  (void)ignored;
  if(n == 0) return 0;
  acquiresleep(&fs->lock);
  tx_begin(fs);
  int r;
  if(lookup(fs, path, &ino, &ip) < 0){
    tx_end(fs, 0);
    r = -1;
  } else
    r = write_common(fs, &ip, off, user_src, src, n);
  releasesleep(&fs->lock);
  return r;
}

int
ext2writeino(int dev, uint64 handle, uint64 off, int user_src, uint64 src, int n)
{
  struct ext2_fs *fs = ext2_by_dev(dev);
  if(fs == 0 || !fs->ready) return -1;
  struct einode ip;
  if(n == 0) return 0;
  acquiresleep(&fs->lock);
  tx_begin(fs);
  int r;
  if(readinode(fs, (uint32)handle, &ip) < 0){
    tx_end(fs, 0);
    r = -1;
  } else
    r = write_common(fs, &ip, off, user_src, src, n);
  releasesleep(&fs->lock);
  return r;
}

static int
mkdir_core(struct ext2_fs *fs, struct einode *pp, const char *name, int namelen, uint32 *out)
{
  struct einode parent = *pp, child;
  uint32 ino, bno;
  if(!fs->ready || e2dirlookup(fs, &parent, name, namelen, &ino) == 0 ||
     mark_write_session_dirty(fs) < 0)
    return -1;
  uint32 group = (parent.ino - 1) / fs->inodes_per_group;
  if(alloc_inode(fs, group, EXT2_S_IFDIR | 0755, &child) < 0)
    return -1;
  child.links = 2;
  bno = get_or_alloc_fileblock(fs, &child, 0);
  if(bno == 0)
    return -1;
  memset(fs->blockbuf, 0, fs->block_size);
  put32(fs->blockbuf, child.ino);
  put16(fs->blockbuf + 4, 12);
  fs->blockbuf[6] = 1; fs->blockbuf[7] = 2; fs->blockbuf[8] = '.';
  put32(fs->blockbuf + 12, parent.ino);
  put16(fs->blockbuf + 16, fs->block_size - 12);
  fs->blockbuf[18] = 2; fs->blockbuf[19] = 2;
  fs->blockbuf[20] = '.'; fs->blockbuf[21] = '.';
  child.size = fs->block_size;
  if(writeblock(fs, bno, fs->blockbuf) < 0 || writeinode(fs, &child) < 0 ||
     e2diradd(fs, &parent, name, namelen, child.ino, 2) < 0)
    return -1;
  if(readinode(fs, parent.ino, &parent) < 0)   // e2diradd may have grown it
    return -1;
  parent.links++;
  *out = child.ino;
  return writeinode(fs, &parent);
}

static int
mkdir_locked(struct ext2_fs *fs, char *path)
{
  struct einode parent, existing;
  uint32 ino;
  char name[EXT2_NAME_MAX + 1];
  int namelen;
  if(!fs->ready || lookup(fs, path, &ino, &existing) == 0 ||
     lookupparent(fs, path, &parent, name, &namelen) < 0)
    return -1;
  return mkdir_core(fs, &parent, name, namelen, &ino);
}

int
ext2mkdir(char *path)
{
  struct ext2_fs *fs = ext2_default();
  if(fs == 0) return -1;
  acquiresleep(&fs->lock);
  tx_begin(fs);
  int r = tx_end(fs, mkdir_locked(fs, path) == 0);
  releasesleep(&fs->lock);
  return r;
}

static int
unlink_core(struct ext2_fs *fs, struct einode *pp, const char *name, int namelen)
{
  struct einode parent = *pp, child;
  uint32 ino;
  if((namelen == 1 && name[0] == '.') ||
     (namelen == 2 && name[0] == '.' && name[1] == '.') ||
     e2dirlookup(fs, &parent, name, namelen, &ino) < 0 ||
     readinode(fs, ino, &child) < 0 || child.links == 0)
    return -1;
  int directory = (child.mode & EXT2_S_IFMT) == EXT2_S_IFDIR;
  if((directory && !e2dirempty(fs, &child)) ||
     mark_write_session_dirty(fs) < 0 ||
     e2dirremove(fs, &parent, name, namelen) < 0)
    return -1;
  if(directory){
    if(readinode(fs, parent.ino, &parent) < 0) return -1;
    if(parent.links) parent.links--;     // child's ".." no longer counts
    if(writeinode(fs, &parent) < 0) return -1;
  }
  return drop_link(fs, &child);
}

static int
unlink_locked(struct ext2_fs *fs, char *path)
{
  struct einode parent;
  char name[EXT2_NAME_MAX + 1];
  int namelen;
  if(lookupparent(fs, path, &parent, name, &namelen) < 0)
    return -1;
  return unlink_core(fs, &parent, name, namelen);
}

int
ext2unlink(char *path)
{
  struct ext2_fs *fs = ext2_default();
  if(fs == 0) return -1;
  acquiresleep(&fs->lock);
  tx_begin(fs);
  int r = tx_end(fs, unlink_locked(fs, path) == 0);
  releasesleep(&fs->lock);
  return r;
}

// Is directory `dir` equal to, or an ancestor of, directory `ino`?
static int
is_ancestor(struct ext2_fs *fs, uint32 dir, uint32 ino)
{
  struct einode cur;
  for(uint32 steps = 0; steps < fs->inodes; steps++){
    if(ino == dir) return 1;
    if(ino == EXT2_ROOT_INO) return 0;
    uint32 up;
    if(readinode(fs, ino, &cur) < 0 || e2dirlookup(fs, &cur, "..", 2, &up) < 0)
      return 1;                          // be conservative on a damaged tree
    if(up == ino) return 0;
    ino = up;
  }
  return 1;
}

static int
rename_core(struct ext2_fs *fs, struct einode *op, const char *oldname, int oldlen,
            struct einode *np, const char *newname, int newlen)
{
  struct einode oldparent = *op, newparent = *np, child, target;
  uint32 ino, tino;
  if((oldlen == 1 && oldname[0] == '.') ||
     (oldlen == 2 && oldname[0] == '.' && oldname[1] == '.') ||
     (newlen == 1 && newname[0] == '.') ||
     (newlen == 2 && newname[0] == '.' && newname[1] == '.') ||
     e2dirlookup(fs, &oldparent, oldname, oldlen, &ino) < 0 ||
     readinode(fs, ino, &child) < 0)
    return -1;
  int isdir = (child.mode & EXT2_S_IFMT) == EXT2_S_IFDIR;
  // A directory cannot be moved below itself: that would detach a cycle.
  if(isdir && is_ancestor(fs, child.ino, newparent.ino))
    return -1;
  int exists = e2dirlookup(fs, &newparent, newname, newlen, &tino) == 0;
  if(exists && tino == child.ino)
    return 0;                            // same object: POSIX no-op
  if(exists){
    if(readinode(fs, tino, &target) < 0) return -1;
    int tdir = (target.mode & EXT2_S_IFMT) == EXT2_S_IFDIR;
    if(isdir != tdir || (tdir && !e2dirempty(fs, &target)))
      return -1;
  }
  if(mark_write_session_dirty(fs) < 0)
    return -1;
  // Point the new name at the child (replacing any target), then drop the old
  // name.  With the journal the whole sequence commits atomically.
  if(exists){
    if(e2dirreplace(fs, &newparent, newname, newlen, child.ino) < 0) return -1;
  } else if(e2diradd(fs, &newparent, newname, newlen, child.ino, isdir ? 2 : 1) < 0)
    return -1;
  if(readinode(fs, oldparent.ino, &oldparent) < 0 ||
     e2dirremove(fs, &oldparent, oldname, oldlen) < 0)
    return -1;
  if(isdir && oldparent.ino != newparent.ino){
    if(e2dirreplace(fs, &child, "..", 2, newparent.ino) < 0) return -1;
    if(readinode(fs, oldparent.ino, &oldparent) < 0) return -1;
    if(oldparent.links) oldparent.links--;
    if(writeinode(fs, &oldparent) < 0) return -1;
    if(readinode(fs, newparent.ino, &newparent) < 0) return -1;
    newparent.links++;
    if(writeinode(fs, &newparent) < 0) return -1;
  }
  if(exists){
    if((target.mode & EXT2_S_IFMT) == EXT2_S_IFDIR){
      if(readinode(fs, newparent.ino, &newparent) < 0) return -1;
      if(newparent.links) newparent.links--;   // replaced dir's ".."
      if(writeinode(fs, &newparent) < 0) return -1;
    }
    if(readinode(fs, tino, &target) < 0 || drop_link(fs, &target) < 0)
      return -1;
  }
  return 0;
}

static int
rename_locked(struct ext2_fs *fs, char *oldpath, char *newpath)
{
  struct einode oldparent, newparent;
  char oldname[EXT2_NAME_MAX + 1], newname[EXT2_NAME_MAX + 1];
  int oldlen, newlen;
  if(lookupparent(fs, oldpath, &oldparent, oldname, &oldlen) < 0 ||
     lookupparent(fs, newpath, &newparent, newname, &newlen) < 0)
    return -1;
  return rename_core(fs, &oldparent, oldname, oldlen, &newparent, newname, newlen);
}

int
ext2rename(char *oldpath, char *newpath)
{
  struct ext2_fs *fs = ext2_default();
  if(fs == 0) return -1;
  if(strncmp(oldpath, newpath, MAXPATH) == 0) return 0;
  acquiresleep(&fs->lock);
  tx_begin(fs);
  int r = tx_end(fs, rename_locked(fs, oldpath, newpath) == 0);
  releasesleep(&fs->lock);
  return r;
}

int
ext2fsync(int dev)
{
  struct ext2_fs *fs = ext2_by_dev(dev);
  if(fs == 0 || !fs->ready) return -1;
  int r;
  acquiresleep(&fs->lock);
  // Each operation already committed synchronously (journal) or wrote its
  // sectors in place; sdflush is the card-level completion barrier.
  r = sdflush();
  releasesleep(&fs->lock);
  return r;
}

// ---------------------------------------------------------------------------
// VFS inode operations (vfs.h).  The VFS keeps one struct inode per ext2
// inode in use, with ip->inum = the ext2 inode number, so ext2 is mounted
// like any other filesystem and can be the root.  Each in-memory inode holds
// a slot in fs->open, which is what keeps an unlinked-but-open file alive
// (drop_link() marks it orphan); evict() releases the slot.

static int
e2fill(struct inode *ip, struct einode *e)
{
  if(inode_supported(e) < 0)
    return -1;
  uint16 kind = e->mode & EXT2_S_IFMT;
  ip->type = kind == EXT2_S_IFDIR ? T_DIR : T_FILE;
  ip->nlink = e->links;
  ip->size = e->size;
  ip->major = ip->minor = 0;
  if(ip->type == T_DIR){
    ip->iop = &ext2_dir_iops;
    ip->i_fop = &ext2_dir_fops;
  } else {
    ip->iop = &ext2_file_iops;
    ip->i_fop = &ext2_file_fops;
  }
  return 0;
}

static int
e2_read_inode(struct inode *ip)
{
  struct ext2_fs *fs = ext2_from_inode(ip);
  if(fs == 0 || !fs->ready) return -1;
  struct einode e;
  int r = -1;
  acquiresleep(&fs->lock);
  if(readinode(fs, ip->inum, &e) == 0 && inode_supported(&e) == 0){
    int s = open_slot(fs, ip->inum);
    if(s < 0)
      for(int i = 0; i < NFILE; i++)
        if(fs->open[i].ref == 0){
          s = i;
          fs->open[s].ino = ip->inum;
          fs->open[s].orphan = 0;
          break;
        }
    if(s >= 0){
      if(e2fill(ip, &e) == 0){
        fs->open[s].ref++;
        r = 0;
      }
    }
  }
  releasesleep(&fs->lock);
  return r;
}

static void
e2_evict(struct inode *ip)
{
  struct ext2_fs *fs = ext2_from_inode(ip);
  if(fs == 0 || !fs->ready) return;
  acquiresleep(&fs->lock);
  int s = open_slot(fs, ip->inum);
  releasesleep(&fs->lock);
  if(s >= 0)
    ext2release(ip->dev, ip->inum); // reclaims it if it is an orphan
}

static struct inode*
e2_lookup(struct inode *dp, char *name)
{
  struct ext2_fs *fs = ext2_from_inode(dp);
  if(fs == 0 || !fs->ready) return 0;
  struct einode d;
  uint32 ino;
  acquiresleep(&fs->lock);
  int ok = readinode(fs, dp->inum, &d) == 0 &&
           inode_supported(&d) == 0 &&
           (d.mode & EXT2_S_IFMT) == EXT2_S_IFDIR &&
           e2dirlookup(fs, &d, name, strlen(name), &ino) == 0;
  releasesleep(&fs->lock);
  return ok ? iget(dp->dev, ino) : 0;
}

static struct inode*
e2_create(struct inode *dp, char *name, short type, short major, short minor)
{
  struct ext2_fs *fs = ext2_from_inode(dp);
  if(fs == 0 || !fs->ready){ iput(dp); return 0; }
  struct inode *ip;
  struct einode d;
  uint32 ino;
  int dev = dp->dev;

  ilock(dp);
  ip = e2_lookup(dp, name);
  iunlock(dp);
  if(ip){
    iput(dp);
    ilock(ip);
    if(type == T_FILE && ip->type == T_FILE)
      return ip;                 // open(O_CREATE) of an existing file
    iunlockput(ip);
    return 0;
  }
  if(type != T_FILE && type != T_DIR){
    iput(dp);                    // no device nodes on ext2
    return 0;
  }
  acquiresleep(&fs->lock);
  tx_begin(fs);
  int ok = readinode(fs, dp->inum, &d) == 0 &&
           (type == T_DIR ? mkdir_core(fs, &d, name, strlen(name), &ino)
                          : create_core(fs, &d, name, strlen(name), &ino)) == 0;
  int r = tx_end(fs, ok);
  int fresh = r == 0 && readinode(fs, dp->inum, &d) == 0;
  releasesleep(&fs->lock);
  if(fresh){                     // directory grew / gained a link
    ilock(dp);                   // (lock order: inode lock before fs->lock)
    (void)e2fill(dp, &d);
    iunlock(dp);
  }
  iput(dp);
  if(r < 0)
    return 0;
  ip = iget(dev, ino);
  ilock(ip);
  return ip;
}

static int
e2_unlink(struct inode *dp, char *name)
{
  struct ext2_fs *fs = ext2_from_inode(dp);
  if(fs == 0 || !fs->ready) return -1;
  struct einode d;
  acquiresleep(&fs->lock);
  tx_begin(fs);
  int ok = readinode(fs, dp->inum, &d) == 0 &&
           unlink_core(fs, &d, name, strlen(name)) == 0;
  int r = tx_end(fs, ok);
  releasesleep(&fs->lock);
  return r;
}

static int
e2_rename(struct inode *odp, char *oname, struct inode *ndp, char *nname)
{
  struct ext2_fs *fs = ext2_from_inode(odp);
  if(fs == 0 || fs != ext2_from_inode(ndp) || !fs->ready) return -1;
  struct einode o, n;
  acquiresleep(&fs->lock);
  tx_begin(fs);
  int ok = readinode(fs, odp->inum, &o) == 0 && readinode(fs, ndp->inum, &n) == 0 &&
           rename_core(fs, &o, oname, strlen(oname), &n, nname, strlen(nname)) == 0;
  int r = tx_end(fs, ok);
  releasesleep(&fs->lock);
  return r;
}

static int
e2_readdir(struct inode *dp, int index, char *name, uint *ino)
{
  struct ext2_fs *fs = ext2_from_inode(dp);
  if(fs == 0 || !fs->ready) return -1;
  struct ext2_user_dirent de;
  struct einode d;
  int r = -1;
  acquiresleep(&fs->lock);
  if(readinode(fs, dp->inum, &d) == 0 && (r = readdir_core(fs, &d, index, &de)) > 0){
    safestrcpy(name, de.name, NAMEMAX);
    *ino = de.inode;
  }
  releasesleep(&fs->lock);
  return r;
}

static int
e2_read(struct inode *ip, int user_dst, uint64 dst, uint64 off, uint n)
{
  struct ext2_fs *fs = ext2_from_inode(ip);
  if(fs == 0 || !fs->ready) return -1;
  if(ip->type == T_DIR)
    return vfs_dir_read(ip, user_dst, dst, off, n, e2_readdir);
  char *page = kalloc();
  int total = 0;
  if(page == 0)
    return -1;
  while(total < (int)n){
    int chunk = n - total > PGSIZE ? PGSIZE : n - total;
    int got = ext2readino(ip->dev, ip->inum, off + total, page, chunk);
    if(got <= 0)
      break;
    if(either_copyout(user_dst, dst + total, page, got) < 0){
      total = total ? total : -1;
      break;
    }
    total += got;
    if(got < chunk)
      break;
  }
  kfree(page);
  return total;
}

static int
e2_write(struct inode *ip, int user_src, uint64 src, uint64 off, uint n)
{
  if(ip->type != T_FILE)
    return -1;
  int r = ext2writeino(ip->dev, ip->inum, off, user_src, src, n);
  if(r > 0 && off + r > ip->size)
    ip->size = off + r;
  return r;
}

static int
e2_truncate(struct inode *ip, uint64 size)
{
  if(ip->type != T_FILE || ext2truncino(ip->dev, ip->inum, size) < 0)
    return -1;
  ip->size = size;
  return 0;
}

static int
e2_fsync(struct inode *ip)
{
  return ext2fsync(ip->dev);
}

static int
e2_file_read_at(struct file *f, int user_dst, uint64 dst, uint64 off, uint n)
{
  return e2_read(f->ip, user_dst, dst, off, n);
}

static int
e2_file_write_at(struct file *f, int user_src, uint64 src, uint64 off,
                 uint n)
{
  return e2_write(f->ip, user_src, src, off, n);
}

static int
e2_file_fsync(struct file *f, int dataonly)
{
  (void)dataonly;
  return e2_fsync(f->ip);
}

static const struct file_operations ext2_file_fops = {
  .fread = generic_file_fread,
  .fwrite = generic_file_fwrite,
  .read_at = e2_file_read_at,
  .write_at = e2_file_write_at,
  .fsync = e2_file_fsync,
  .poll = generic_file_poll,
};

static const struct file_operations ext2_dir_fops = {
  .fread = generic_file_fread,
  .read_at = e2_file_read_at,
  .poll = generic_file_poll,
};

static int
ext2_put_super(struct super_block *sb)
{
  struct ext2_fs *fs = sb ? sb->priv : 0;
  struct blkdev *bd = sb ? blkdev_get(sb->dev) : 0;
  int r = -1;
  if(fs == 0 || bd == 0)
    return -1;

  acquiresleep(&ext2_mount_lock);
  acquiresleep(&fs->lock);
  if(!fs->ready || fs->dev != sb->dev || xj_active())
    goto out;
  // The clean flag is the final filesystem metadata transaction.  Only after
  // it is durable may the external log and the cached home blocks disappear.
  if(mark_write_session_clean(fs) < 0 || binvalidate(fs->dev) < 0)
    goto out;
  if(fs->journal){
    if(xj_shutdown(bd) < 0)
      goto out;
    fs->journal = 0;
    xj_owner_dev = 0;
    // Recovery during shutdown can have checkpointed a pending transaction.
    if(binvalidate(fs->dev) < 0)
      goto out;
  }
  if(blkdev_set_bsize(fs->dev, SECTOR) < 0)
    goto out;
  ext2_drop_instance(fs);
  r = 0;
out:
  releasesleep(&fs->lock);
  releasesleep(&ext2_mount_lock);
  return r;
}

// Superblock bootstrap table. e2_read_inode learns the inode type and e2fill
// switches to the corresponding directory or regular-file tables.
static struct inode_ops ext2_iops = {
  .read_inode = e2_read_inode,
  .evict = e2_evict,
};

static struct inode_ops ext2_file_iops = {
  .read_inode = e2_read_inode,
  .evict = e2_evict,
  .truncate = e2_truncate,
};

static struct inode_ops ext2_dir_iops = {
  .read_inode = e2_read_inode,
  .evict = e2_evict,
  .lookup = e2_lookup,
  .create = e2_create,
  .unlink = e2_unlink,
  .rename = e2_rename,
};

// Make the ext2 partition known to the VFS as the filesystem on block
// device dev (mmcblk0pN).
int
ext2_register_super(int dev, int readonly)
{
  struct ext2_fs *fs = ext2_by_dev(dev);
  if(fs == 0 || !fs->ready)
    return -1;
  if(fs_super_register(dev, "ext2", &ext2_iops, EXT2_ROOT_INO,
                       readonly, 0, 0, fs) < 0)
    return -1;
  return fs_super_set_put_super(dev, ext2_put_super);
}
