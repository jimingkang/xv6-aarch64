// File system implementation.  Five layers:
//   + Blocks: allocator for raw disk blocks.
//   + Log: crash recovery for multi-step updates.
//   + Files: inode allocator, reading, writing, metadata.
//   + Directories: inode with special contents (list of other inodes!)
//   + Names: paths like /usr/rtm/xv6/fs.c for convenient naming.
//
// This file contains the low-level file system manipulation
// routines.  The (higher-level) system call implementations
// are in sysfile.c.

#include "types.h"
#include "aarch64.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "file.h"
#include "blkdev.h"
#include "vfs.h"

#define min(a, b) ((a) < (b) ? (a) : (b))
// One on-disk xv6 superblock per mounted xv6 filesystem, indexed by block
// device number: the root on dev 1 and the RAM-disk instances (rootfs,
// devtmpfs).
static struct superblock sbs[NBLKDEV];
void fs_mountinit(void);
static struct inode_ops xv6_iops;

// ---------------------------------------------------------------------------
// VFS super blocks: one per mounted filesystem instance of any type,
// indexed by device number (see vfs.h).

static struct super_block supers[NSUPER];

struct super_block*
getsuper(uint dev)
{
  if(dev >= NSUPER || !supers[dev].used)
    return 0;
  return &supers[dev];
}

// Register (or re-register) the filesystem instance on dev.
int
fs_super_register(int dev, char *type, struct inode_ops *iop, uint rootino,
                  int readonly, int logged, int pathbased, void *priv)
{
  if(dev <= 0 || dev >= NSUPER)
    return -1;
  struct super_block *sb = &supers[dev];
  sb->dev = dev;
  safestrcpy(sb->type, type, sizeof(sb->type));
  sb->iop = iop;
  sb->rootino = rootino;
  sb->readonly = readonly;
  sb->logged = logged;
  sb->pathbased = pathbased;
  sb->priv = priv;
  __sync_synchronize();
  sb->used = 1;
  return 0;
}

static struct superblock*
getsb(uint dev)
{
  if(dev >= NBLKDEV || sbs[dev].magic != FSMAGIC)
    panic("getsb: device has no xv6 filesystem");
  return &sbs[dev];
}

// Read and check the superblock of dev (the "fill_super" step of a mount).
int
fs_readsuper(int dev)
{
  struct buf *bp;
  struct superblock s;

  if(dev <= 0 || dev >= NBLKDEV)
    return -1;
  bp = bread(dev, 1);
  if(bp->error){
    brelse(bp);
    return -1;
  }
  memmove(&s, bp->data, sizeof(s));
  brelse(bp);
  if(s.magic != FSMAGIC)
    return -1;
  sbs[dev] = s;
  fs_super_register(dev, "xv6fs", &xv6_iops, ROOTINO, 0, 1, 0, 0);
  return 0;
}

// Mount-time work for the disk root: superblock, then log recovery.
// Only the root has a log; RAM-disk instances write through (log_write()).
int
fsinit(int dev) {
  if(fs_readsuper(dev) < 0)
    return -1;
  initlog(dev, &sbs[dev]);
  return 0;
}

// Zero a block.
static void
bzero(int dev, int bno)
{
  struct buf *bp;

  bp = bread(dev, bno);
  memset(bp->data, 0, BSIZE);
  log_write(bp);
  brelse(bp);
}

// Blocks.

// Allocate a zeroed disk block.
static uint
balloc(uint dev)
{
  int b, bi, m;
  struct buf *bp;

  struct superblock *sb = getsb(dev);
  bp = 0;
  for(b = 0; b < sb->size; b += BPB){
    bp = bread(dev, BBLOCK(b, (*sb)));
    for(bi = 0; bi < BPB && b + bi < sb->size; bi++){
      m = 1 << (bi % 8);
      if((bp->data[bi/8] & m) == 0){  // Is block free?
        bp->data[bi/8] |= m;  // Mark block in use.
        log_write(bp);
        brelse(bp);
        bzero(dev, b + bi);
        return b + bi;
      }
    }
    brelse(bp);
  }
  panic("balloc: out of blocks");
}

// Free a disk block.
static void
bfree(int dev, uint b)
{
  struct buf *bp;
  int bi, m;

  bp = bread(dev, BBLOCK(b, (*getsb(dev))));
  bi = b % BPB;
  m = 1 << (bi % 8);
  if((bp->data[bi/8] & m) == 0)
    panic("freeing free block");
  bp->data[bi/8] &= ~m;
  log_write(bp);
  brelse(bp);
}

// Inodes.
//
// An inode describes a single unnamed file.
// The inode disk structure holds metadata: the file's type,
// its size, the number of links referring to it, and the
// list of blocks holding the file's content.
//
// The inodes are laid out sequentially on disk at
// sb.startinode. Each inode has a number, indicating its
// position on the disk.
//
// The kernel keeps a table of in-use inodes in memory
// to provide a place for synchronizing access
// to inodes used by multiple processes. The in-memory
// inodes include book-keeping information that is
// not stored on disk: ip->ref and ip->valid.
//
// An inode and its in-memory representation go through a
// sequence of states before they can be used by the
// rest of the file system code.
//
// * Allocation: an inode is allocated if its type (on disk)
//   is non-zero. ialloc() allocates, and iput() frees if
//   the reference and link counts have fallen to zero.
//
// * Referencing in table: an entry in the inode table
//   is free if ip->ref is zero. Otherwise ip->ref tracks
//   the number of in-memory pointers to the entry (open
//   files and current directories). iget() finds or
//   creates a table entry and increments its ref; iput()
//   decrements ref.
//
// * Valid: the information (type, size, &c) in an inode
//   table entry is only correct when ip->valid is 1.
//   ilock() reads the inode from
//   the disk and sets ip->valid, while iput() clears
//   ip->valid if ip->ref has fallen to zero.
//
// * Locked: file system code may only examine and modify
//   the information in an inode and its content if it
//   has first locked the inode.
//
// Thus a typical sequence is:
//   ip = iget(dev, inum)
//   ilock(ip)
//   ... examine and modify ip->xxx ...
//   iunlock(ip)
//   iput(ip)
//
// ilock() is separate from iget() so that system calls can
// get a long-term reference to an inode (as for an open file)
// and only lock it for short periods (e.g., in read()).
// The separation also helps avoid deadlock and races during
// pathname lookup. iget() increments ip->ref so that the inode
// stays in the table and pointers to it remain valid.
//
// Many internal file system functions expect the caller to
// have locked the inodes involved; this lets callers create
// multi-step atomic operations.
//
// The itable.lock spin-lock protects the allocation of itable
// entries. Since ip->ref indicates whether an entry is free,
// and ip->dev and ip->inum indicate which i-node an entry
// holds, one must hold itable.lock while using any of those fields.
//
// An ip->lock sleep-lock protects all ip-> fields other than ref,
// dev, and inum.  One must hold ip->lock in order to
// read or write that inode's ip->valid, ip->size, ip->type, &c.

struct {
  struct spinlock lock;
  struct inode inode[NINODE];
} itable;

void
iinit()
{
  int i = 0;
  
  initlock(&itable.lock, "itable");
  for(i = 0; i < NINODE; i++) {
    initsleeplock(&itable.inode[i].lock, "inode");
  }
  loginit();
  fs_mountinit();
}


// Allocate an inode on device dev.
// Mark it as allocated by  giving it type type.
// Returns an unlocked but allocated and referenced inode.
struct inode*
ialloc(uint dev, short type)
{
  int inum;
  struct buf *bp;
  struct dinode *dip;

  struct superblock *sb = getsb(dev);
  for(inum = 1; inum < sb->ninodes; inum++){
    bp = bread(dev, IBLOCK(inum, (*sb)));
    dip = (struct dinode*)bp->data + inum%IPB;
    if(dip->type == 0){  // a free inode
      memset(dip, 0, sizeof(*dip));
      dip->type = type;
      log_write(bp);   // mark it allocated on the disk
      brelse(bp);
      return iget(dev, inum);
    }
    brelse(bp);
  }
  panic("ialloc: no inodes");
}

// Copy a modified in-memory inode to disk.
// Must be called after every change to an ip->xxx field
// that lives on disk.
// Caller must hold ip->lock.
void
iupdate(struct inode *ip)
{
  struct buf *bp;
  struct dinode *dip;

  bp = bread(ip->dev, IBLOCK(ip->inum, (*getsb(ip->dev))));
  dip = (struct dinode*)bp->data + ip->inum%IPB;
  dip->type = ip->type;
  dip->major = ip->major;
  dip->minor = ip->minor;
  dip->nlink = ip->nlink;
  dip->size = ip->size;
  memmove(dip->addrs, ip->addrs, sizeof(ip->addrs));
  log_write(bp);
  brelse(bp);
}

// Find the inode with number inum on device dev
// and return the in-memory copy. Does not lock
// the inode and does not read it from disk.
// Find or make the in-memory inode (dev, inum).  Path-based filesystems
// (FAT32, procfs, netfs) derive inum from the path and also compare the
// path, so a hash collision cannot alias two files.
struct inode*
iget_path(uint dev, uint inum, char *path)
{
  struct inode *ip, *empty;
  struct super_block *sb = getsuper(dev);
  int bypath = sb && sb->pathbased && path;

  acquire(&itable.lock);

  // Is the inode already in the table?
  empty = 0;
  for(ip = &itable.inode[0]; ip < &itable.inode[NINODE]; ip++){
    if(ip->ref > 0 && ip->dev == dev && ip->inum == inum &&
       (!bypath || strncmp(ip->path, path, MAXPATH) == 0)){
      ip->ref++;
      release(&itable.lock);
      return ip;
    }
    if(empty == 0 && ip->ref == 0)    // Remember empty slot.
      empty = ip;
  }

  // Recycle an inode entry.
  if(empty == 0)
    panic("iget: no inodes");

  ip = empty;
  ip->dev = dev;
  ip->inum = inum;
  ip->ref = 1;
  ip->valid = 0;
  ip->iop = sb ? sb->iop : &xv6_iops;
  if(bypath)
    safestrcpy(ip->path, path, MAXPATH);
  else
    ip->path[0] = 0;
  release(&itable.lock);

  return ip;
}

struct inode*
iget(uint dev, uint inum)
{
  return iget_path(dev, inum, 0);
}

// Increment reference count for ip.
// Returns ip to enable ip = idup(ip1) idiom.
struct inode*
idup(struct inode *ip)
{
  acquire(&itable.lock);
  ip->ref++;
  release(&itable.lock);
  return ip;
}

// Lock the given inode.
// Reads the inode from disk if necessary.
static int
xv6_read_inode(struct inode *ip)
{
  struct buf *bp;
  struct dinode *dip;

  bp = bread(ip->dev, IBLOCK(ip->inum, (*getsb(ip->dev))));
  dip = (struct dinode*)bp->data + ip->inum%IPB;
  ip->type = dip->type;
  ip->major = dip->major;
  ip->minor = dip->minor;
  ip->nlink = dip->nlink;
  ip->size = dip->size;
  memmove(ip->addrs, dip->addrs, sizeof(ip->addrs));
  brelse(bp);
  if(ip->type == 0)
    panic("ilock: no type");
  return 0;
}

void
ilock(struct inode *ip)
{
  if(ip == 0 || ip->ref < 1)
    panic("ilock");

  acquiresleep(&ip->lock);

  if(ip->valid == 0){
    if(ip->iop->read_inode(ip) < 0){
      // The object vanished behind our back (path-based filesystems):
      // present an empty, unlinked file instead of crashing.
      ip->type = T_FILE;
      ip->nlink = 0;
      ip->size = 0;
      ip->major = ip->minor = 0;
    }
    ip->valid = 1;
  }
}

// Unlock the given inode.
void
iunlock(struct inode *ip)
{
  if(ip == 0 || !holdingsleep(&ip->lock) || ip->ref < 1)
    panic("iunlock");

  releasesleep(&ip->lock);
}

// Drop a reference to an in-memory inode.
// If that was the last reference, the inode table entry can
// be recycled.
// If that was the last reference and the inode has no links
// to it, free the inode (and its content) on disk.
// All calls to iput() must be inside a transaction in
// case it has to free the inode.
// xv6fs: an inode with no links and no references is truncated and freed.
static void
xv6_evict(struct inode *ip)
{
  if(ip->nlink != 0)
    return;
  itrunc(ip);
  ip->type = 0;
  iupdate(ip);
}

void
iput(struct inode *ip)
{
  acquire(&itable.lock);

  if(ip->ref == 1 && ip->valid && ip->iop->evict){
    // Last reference: let the filesystem release it (xv6fs frees unlinked
    // inodes, ext2 reclaims orphans).
    // ip->ref == 1 means no other process can have ip locked,
    // so this acquiresleep() won't block (or deadlock).
    acquiresleep(&ip->lock);

    release(&itable.lock);

    ip->iop->evict(ip);
    ip->valid = 0;

    releasesleep(&ip->lock);

    acquire(&itable.lock);
  }

  ip->ref--;
  release(&itable.lock);
}

// Common idiom: unlock, then put.
void
iunlockput(struct inode *ip)
{
  iunlock(ip);
  iput(ip);
}

// Inode content
//
// The content (data) associated with each inode is stored
// in blocks on the disk. The first NDIRECT block numbers
// are listed in ip->addrs[].  The next NINDIRECT blocks are
// listed in block ip->addrs[NDIRECT].

// Return the disk block address of the nth block in inode ip.
// If there is no such block, bmap allocates one.
static uint
bmap(struct inode *ip, uint bn)
{
  uint addr, *a;
  struct buf *bp;

  if(bn < NDIRECT){
    if((addr = ip->addrs[bn]) == 0)
      ip->addrs[bn] = addr = balloc(ip->dev);
    return addr;
  }
  bn -= NDIRECT;

  if(bn < NINDIRECT){
    // Load indirect block, allocating if necessary.
    if((addr = ip->addrs[NDIRECT]) == 0)
      ip->addrs[NDIRECT] = addr = balloc(ip->dev);
    bp = bread(ip->dev, addr);
    a = (uint*)bp->data;
    if((addr = a[bn]) == 0){
      a[bn] = addr = balloc(ip->dev);
      log_write(bp);
    }
    brelse(bp);
    return addr;
  }

  panic("bmap: out of range");
}

// Truncate inode (discard contents).
// Caller must hold ip->lock.
void
itrunc(struct inode *ip)
{
  int i, j;
  struct buf *bp;
  uint *a;

  for(i = 0; i < NDIRECT; i++){
    if(ip->addrs[i]){
      bfree(ip->dev, ip->addrs[i]);
      ip->addrs[i] = 0;
    }
  }

  if(ip->addrs[NDIRECT]){
    bp = bread(ip->dev, ip->addrs[NDIRECT]);
    a = (uint*)bp->data;
    for(j = 0; j < NINDIRECT; j++){
      if(a[j])
        bfree(ip->dev, a[j]);
    }
    brelse(bp);
    bfree(ip->dev, ip->addrs[NDIRECT]);
    ip->addrs[NDIRECT] = 0;
  }

  ip->size = 0;
  iupdate(ip);
}

// Copy stat information from inode.
// Caller must hold ip->lock.
void
stati(struct inode *ip, struct stat *st)
{
  st->dev = ip->dev;
  st->ino = ip->inum;
  st->type = ip->type;
  st->nlink = ip->nlink;
  st->size = ip->size;
}

// Read data from inode.
// Caller must hold ip->lock.
// If user_dst==1, then dst is a user virtual address;
// otherwise, dst is a kernel address.
int
readi(struct inode *ip, int user_dst, uint64 dst, uint off, uint n)
{
  uint tot, m;
  struct buf *bp;

  if(off > ip->size || off + n < off)
    return 0;
  if(off + n > ip->size)
    n = ip->size - off;

  for(tot=0; tot<n; tot+=m, off+=m, dst+=m){
    bp = bread(ip->dev, bmap(ip, off/BSIZE));
    m = min(n - tot, BSIZE - off%BSIZE);
    if(either_copyout(user_dst, dst, bp->data + (off % BSIZE), m) == -1) {
      brelse(bp);
      tot = -1;
      break;
    }
    brelse(bp);
  }
  return tot;
}

// Write data to inode.
// Caller must hold ip->lock.
// If user_src==1, then src is a user virtual address;
// otherwise, src is a kernel address.
// Returns the number of bytes successfully written.
// If the return value is less than the requested n,
// there was an error of some kind.
int
writei(struct inode *ip, int user_src, uint64 src, uint off, uint n)
{
  uint tot, m;
  struct buf *bp;

  if(off > ip->size || off + n < off)
    return -1;
  if(off + n > MAXFILE*BSIZE)
    return -1;

  for(tot=0; tot<n; tot+=m, off+=m, src+=m){
    bp = bread(ip->dev, bmap(ip, off/BSIZE));
    m = min(n - tot, BSIZE - off%BSIZE);
    if(either_copyin(bp->data + (off % BSIZE), user_src, src, m) == -1) {
      brelse(bp);
      break;
    }
    log_write(bp);
    brelse(bp);
  }

  if(off > ip->size)
    ip->size = off;

  // write the i-node back to disk even if the size didn't change
  // because the loop above might have called bmap() and added a new
  // block to ip->addrs[].
  iupdate(ip);

  return tot;
}

// Directories

int
namecmp(const char *s, const char *t)
{
  return strncmp(s, t, DIRSIZ);
}

// Look for a directory entry in a directory.
// If found, set *poff to byte offset of entry.
struct inode*
dirlookup(struct inode *dp, char *name, uint *poff)
{
  uint off, inum;
  struct dirent de;

  if(dp->type != T_DIR)
    panic("dirlookup not DIR");

  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlookup read");
    if(de.inum == 0)
      continue;
    if(namecmp(name, de.name) == 0){
      // entry matches path element
      if(poff)
        *poff = off;
      inum = de.inum;
      return iget(dp->dev, inum);
    }
  }

  return 0;
}

// Write a new directory entry (name, inum) into the directory dp.
int
dirlink(struct inode *dp, char *name, uint inum)
{
  int off;
  struct dirent de;
  struct inode *ip;

  // Check that name is not present.
  if((ip = dirlookup(dp, name, 0)) != 0){
    iput(ip);
    return -1;
  }

  // Look for an empty dirent.
  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlink read");
    if(de.inum == 0)
      break;
  }

  strncpy(de.name, name, DIRSIZ);
  de.inum = inum;
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("dirlink");

  return 0;
}

// Paths

// Copy the next path element from path into name.
// Return a pointer to the element following the copied one.
// The returned path has no leading slashes,
// so the caller can check *path=='\0' to see if the name is the last one.
// If no name to remove, return 0.
//
// Examples:
//   skipelem("a/bb/c", name) = "bb/c", setting name = "a"
//   skipelem("///a//bb", name) = "bb", setting name = "a"
//   skipelem("a", name) = "", setting name = "a"
//   skipelem("", name) = skipelem("////", name) = 0
//
static char*
skipelem(char *path, char *name)
{
  char *s;
  int len;

  while(*path == '/')
    path++;
  if(*path == 0)
    return 0;
  s = path;
  while(*path != '/' && *path != 0)
    path++;
  len = path - s;
  if(len >= NAMEMAX)
    len = NAMEMAX - 1;
  memmove(name, s, len);
  name[len] = 0;
  while(*path == '/')
    path++;
  return path;
}

// Mounts of xv6 filesystems (compare Linux struct mount / vfsmount).
//
// A mount says "the directory mp is covered by the root directory of the
// filesystem on dev".  namex() crosses a mount when a lookup lands on a
// covered directory (follow_mount) and climbs back out of a mounted root
// on ".." (follow_dotdot).  fs_root is the namespace root "/": at boot it is
// the rootfs RAM disk; do_mounts.c later moves the real root over it and
// chroots there, exactly like Linux's prepare_namespace().  xv6 has one
// mount namespace, so "chroot" changes the root for every process.
//
// The table holds every mounted filesystem, of any type: each inode carries
// its filesystem's operations (vfs.h), so the same walk crosses from xv6fs
// into ext2, FAT32 or procfs.

#define NFSMOUNT 8

static struct {
  struct spinlock lock;
  struct fsmount {
    int used;
    struct inode *mp;       // covered directory (holds a reference)
    uint dev;               // mounted filesystem; its root is (dev, ROOTINO)
    char fstype[12];
    char source[24];
    char path[MAXPATH];     // where it appears in the namespace, for display
  } m[NFSMOUNT];
} mtab;

static struct inode *fs_root;   // "/", see above

void
fs_mountinit(void)
{
  initlock(&mtab.lock, "mtab");
}

// The inode namex() starts absolute paths from.  Before do_mounts.c has
// set up the namespace (and for the legacy path), it is the disk root.
static struct inode*
rootdir(void)
{
  return fs_root ? idup(fs_root) : iget(ROOTDEV, ROOTINO);
}

// Is ip the root directory of its filesystem?
static int
isfsroot(struct inode *ip)
{
  struct super_block *sb = getsuper(ip->dev);
  return ip->inum == (sb ? sb->rootino : ROOTINO);
}

static struct fsmount*
mount_covering(uint dev, uint inum)
{
  for(int i = 0; i < NFSMOUNT; i++)
    if(mtab.m[i].used && mtab.m[i].mp->dev == dev &&
       mtab.m[i].mp->inum == inum)
      return &mtab.m[i];
  return 0;
}

static struct fsmount*
mount_of_dev(uint dev)
{
  for(int i = 0; i < NFSMOUNT; i++)
    if(mtab.m[i].used && mtab.m[i].dev == dev)
      return &mtab.m[i];
  return 0;
}

// If ip is covered by a mount, replace it with the mounted root (repeat for
// stacked mounts).  Consumes the caller's reference to ip.
static struct inode*
follow_mount(struct inode *ip)
{
  for(;;){
    acquire(&mtab.lock);
    struct fsmount *m = mount_covering(ip->dev, ip->inum);
    uint dev = m ? m->dev : 0;
    release(&mtab.lock);
    if(m == 0)
      return ip;
    iput(ip);
    ip = fs_dev_root(dev);
  }
}

// Before looking up "..": stay put at the namespace root; at the root of a
// mounted filesystem, step down to the covered directory first so ".." is
// looked up in the parent filesystem.  Consumes the reference to ip.
static struct inode*
follow_dotdot(struct inode *ip)
{
  for(;;){
    if(fs_root && ip->dev == fs_root->dev && ip->inum == fs_root->inum)
      return ip;
    if(!isfsroot(ip))
      return ip;
    acquire(&mtab.lock);
    struct fsmount *m = mount_of_dev(ip->dev);
    struct inode *mp = m ? idup(m->mp) : 0;
    release(&mtab.lock);
    if(mp == 0)
      return ip;
    iput(ip);
    ip = mp;
  }
}

// Root directory of the filesystem on dev (referenced, unlocked).
struct inode*
fs_dev_root(int dev)
{
  struct super_block *sb = getsuper(dev);
  return iget_path(dev, sb ? sb->rootino : ROOTINO, "/");
}

int
fs_is_mountpoint(struct inode *ip)
{
  acquire(&mtab.lock);
  int r = mount_covering(ip->dev, ip->inum) != 0 ||
          (isfsroot(ip) && mount_of_dev(ip->dev) != 0);
  release(&mtab.lock);
  return r;
}

// Attach the xv6 filesystem on dev (superblock already read) at directory
// mp.  Takes over the caller's reference to mp.
int
fs_mount(struct inode *mp, int dev, char *fstype, char *source, char *path)
{
  ilock(mp);
  int isdir = mp->type == T_DIR;
  iunlock(mp);
  if(!isdir)
    return -1;
  acquire(&mtab.lock);
  if(mount_covering(mp->dev, mp->inum) || mount_of_dev(dev)){
    release(&mtab.lock);
    return -1;
  }
  for(int i = 0; i < NFSMOUNT; i++){
    struct fsmount *m = &mtab.m[i];
    if(m->used)
      continue;
    m->used = 1;
    m->mp = mp;
    m->dev = dev;
    safestrcpy(m->fstype, fstype, sizeof(m->fstype));
    safestrcpy(m->source, source, sizeof(m->source));
    safestrcpy(m->path, path, sizeof(m->path));
    release(&mtab.lock);
    return 0;
  }
  release(&mtab.lock);
  return -1;
}

// MS_MOVE: the filesystem whose root directory is "from" is detached from
// where it is mounted and re-attached on directory "to", which is reached
// as topath.  Mounts below it (devtmpfs on /root/dev) move along; only
// their display paths change.  Consumes the reference to "to".
int
fs_move_mount(struct inode *from, struct inode *to, char *topath)
{
  struct inode *old;
  char oldpath[MAXPATH];
  acquire(&mtab.lock);
  struct fsmount *m = isfsroot(from) ? mount_of_dev(from->dev) : 0;
  if(m == 0 || mount_covering(to->dev, to->inum)){
    release(&mtab.lock);
    return -1;
  }
  old = m->mp;
  m->mp = to;
  safestrcpy(oldpath, m->path, sizeof(oldpath));
  int n = strlen(oldpath);
  for(int i = 0; i < NFSMOUNT; i++){
    struct fsmount *c = &mtab.m[i];
    if(!c->used || strncmp(c->path, oldpath, n) != 0 ||
       (c->path[n] != 0 && c->path[n] != '/'))
      continue;
    char rest[MAXPATH];
    safestrcpy(rest, c->path + n, sizeof(rest));
    safestrcpy(c->path, topath, sizeof(c->path));
    if(rest[0]){
      int t = strlen(c->path);
      if(t == 1)                  // topath "/": avoid "//dev"
        t = 0;
      safestrcpy(c->path + t, rest, sizeof(c->path) - t);
    }
  }
  release(&mtab.lock);
  iput(old);
  return 0;
}

// chroot for the (single) namespace.
void
fs_set_root(struct inode *ip)
{
  struct inode *old = fs_root;
  fs_root = idup(ip);
  if(old)
    iput(old);
}

static char*
mstr(char *p, char *e, char *s)
{
  while(*s && p < e)
    *p++ = *s++;
  return p;
}

// /proc/mounts lines for the xv6-inode mounts ("source target type opts").
int
fs_mounts_format(char *buf, int n)
{
  char *p = buf, *e = buf + n;
  acquire(&mtab.lock);
  for(int i = 0; i < NFSMOUNT; i++){
    struct fsmount *m = &mtab.m[i];
    if(!m->used)
      continue;
    p = mstr(p, e, m->source);
    p = mstr(p, e, " ");
    p = mstr(p, e, m->path);
    p = mstr(p, e, " ");
    p = mstr(p, e, m->fstype);
    struct super_block *sb = getsuper(m->dev);
    p = mstr(p, e, sb && sb->readonly ? " ro 0 0\n" : " rw 0 0\n");
  }
  release(&mtab.lock);
  return p - buf;
}

// Look up and return the inode for a path name.
// If parent != 0, return the inode for the parent and copy the final
// path element into name, which must have room for DIRSIZ bytes.
// Must be called inside a transaction since it calls iput().
static struct inode*
namex(char *path, int nameiparent, char *name)
{
  struct inode *ip, *next;

  if(*path == '/')
    ip = rootdir();
  else
    ip = idup(myproc()->cwd);

  while((path = skipelem(path, name)) != 0){
    if(namecmp(name, "..") == 0)
      ip = follow_dotdot(ip);
    ilock(ip);
    if(ip->type != T_DIR){
      iunlockput(ip);
      return 0;
    }
    if(nameiparent && *path == '\0'){
      // Stop one level early.
      iunlock(ip);
      return ip;
    }
    if((next = ip->iop->lookup(ip, name)) == 0){
      iunlockput(ip);
      return 0;
    }
    iunlockput(ip);
    ip = follow_mount(next);
  }
  if(nameiparent){
    iput(ip);
    return 0;
  }
  return ip;
}

struct inode*
namei(char *path)
{
  char name[NAMEMAX];
  return namex(path, 0, name);
}

struct inode*
nameiparent(char *path, char *name)
{
  return namex(path, 1, name);
}

// Create name in directory dp (a referenced, unlocked inode; the reference
// is consumed).  Returns the new inode locked, or for T_FILE an existing
// file/device; 0 on failure.  Shared by open(O_CREATE)/mkdir/mknod and the
// kernel's own node creation (rootfs, devtmpfs).  Caller is in a
// transaction.
static struct inode*
xv6_create(struct inode *dp, char *name, short type, short major, short minor)
{
  struct inode *ip;

  ilock(dp);

  if((ip = dirlookup(dp, name, 0)) != 0){
    iunlockput(dp);
    ilock(ip);
    if(type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE))
      return ip;
    iunlockput(ip);
    return 0;
  }

  if((ip = ialloc(dp->dev, type)) == 0)
    panic("create: ialloc");

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if(type == T_DIR){  // Create . and .. entries.
    dp->nlink++;  // for ".."
    iupdate(dp);
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      panic("create dots");
  }

  if(dirlink(dp, name, ip->inum) < 0)
    panic("create: dirlink");

  iunlockput(dp);

  return ip;
}

// ---------------------------------------------------------------------------
// The rest of the xv6fs inode operations.

static struct inode*
xv6_lookup(struct inode *dp, char *name)
{
  return dirlookup(dp, name, 0);
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for(off=2*sizeof(de); off<dp->size; off+=sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if(de.inum != 0)
      return 0;
  }
  return 1;
}

static int
xv6_unlink(struct inode *dp, char *name)
{
  struct inode *ip;
  struct dirent de;
  uint off;

  ilock(dp);
  // Cannot unlink "." or "..".
  if(namecmp(name, ".") == 0 || namecmp(name, "..") == 0 ||
     (ip = dirlookup(dp, name, &off)) == 0){
    iunlock(dp);
    return -1;
  }
  ilock(ip);
  if(ip->nlink < 1)
    panic("unlink: nlink < 1");
  if(ip->type == T_DIR && !isdirempty(ip)){
    iunlockput(ip);
    iunlock(dp);
    return -1;
  }
  memset(&de, 0, sizeof(de));
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if(ip->type == T_DIR){
    dp->nlink--;
    iupdate(dp);
  }
  iunlock(dp);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  return 0;
}

static int
xv6_link(struct inode *dp, char *name, struct inode *ip)
{
  ilock(ip);
  if(ip->type == T_DIR){
    iunlock(ip);
    return -1;
  }
  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  ilock(dp);
  if(dirlink(dp, name, ip->inum) < 0){
    iunlock(dp);
    ilock(ip);
    ip->nlink--;
    iupdate(ip);
    iunlock(ip);
    return -1;
  }
  iunlock(dp);
  return 0;
}

// Rename a file (not a directory) within one xv6 filesystem: add the new
// name, then clear the old entry, in the caller's single log transaction.
static int
xv6_rename(struct inode *odp, char *oname, struct inode *ndp, char *nname)
{
  struct inode *ip, *t;
  struct dirent de;
  uint off;

  ilock(odp);
  ip = dirlookup(odp, oname, &off);
  iunlock(odp);
  if(ip == 0)
    return -1;
  ilock(ip);
  int isdir = ip->type == T_DIR;
  iunlock(ip);
  if(isdir){
    iput(ip);
    return -1;
  }
  ilock(ndp);
  if((t = dirlookup(ndp, nname, 0)) != 0){
    iunlock(ndp);
    int same = t == ip;
    iput(t);
    iput(ip);
    return same ? 0 : -1;        // replacing an existing name: not supported
  }
  if(dirlink(ndp, nname, ip->inum) < 0){
    iunlock(ndp);
    iput(ip);
    return -1;
  }
  iunlock(ndp);
  ilock(odp);
  if(readi(odp, 0, (uint64)&de, off, sizeof(de)) == sizeof(de) &&
     de.inum == ip->inum){
    memset(&de, 0, sizeof(de));
    if(writei(odp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("rename: writei");
  }
  iunlock(odp);
  iput(ip);
  return 0;
}

static int
xv6_read(struct inode *ip, int user_dst, uint64 dst, uint64 off, uint n)
{
  if(off > 0xffffffffULL || (uint64)n > 0x100000000ULL - off)
    return -1;
  return readi(ip, user_dst, dst, off, n);
}

static int
xv6_write(struct inode *ip, int user_src, uint64 src, uint64 off, uint n)
{
  if(off > 0xffffffffULL || (uint64)n > 0x100000000ULL - off)
    return -1;
  return writei(ip, user_src, src, off, n);
}

static int
xv6_truncate(struct inode *ip, uint64 size)
{
  if(ip->type != T_FILE || size != 0)
    return -1;
  itrunc(ip);
  return 0;
}

static int
xv6_fsync(struct inode *ip)
{
  // Writes commit synchronously through the log; sdflush is the card's
  // programming barrier.
  return sdflush();
}

static struct inode_ops xv6_iops = {
  .read_inode = xv6_read_inode,
  .evict = xv6_evict,
  .lookup = xv6_lookup,
  .create = xv6_create,
  .unlink = xv6_unlink,
  .link = xv6_link,
  .rename = xv6_rename,
  .read = xv6_read,
  .write = xv6_write,
  .truncate = xv6_truncate,
  .fsync = xv6_fsync,
};

// ---------------------------------------------------------------------------
// Generic entry points used by the system calls and the kernel.  They check
// that the filesystem is writable and supports the operation.

static int
fs_writable(struct inode *ip)
{
  struct super_block *sb = getsuper(ip->dev);
  return !(sb && sb->readonly);
}

// Create name in directory dp (referenced, unlocked; reference consumed).
// Returns the inode locked, or 0.  Caller is in a transaction.
struct inode*
fs_create(struct inode *dp, char *name, short type, short major, short minor)
{
  if(!fs_writable(dp) || dp->iop->create == 0){
    iput(dp);
    return 0;
  }
  return dp->iop->create(dp, name, type, major, minor);
}

int
fs_unlink(struct inode *dp, char *name)
{
  if(!fs_writable(dp) || dp->iop->unlink == 0)
    return -1;
  return dp->iop->unlink(dp, name);
}

int
fs_link(struct inode *dp, char *name, struct inode *ip)
{
  if(dp->dev != ip->dev || !fs_writable(dp) || dp->iop->link == 0)
    return -1;
  return dp->iop->link(dp, name, ip);
}

int
fs_rename(struct inode *odp, char *oname, struct inode *ndp, char *nname)
{
  if(odp->dev != ndp->dev || !fs_writable(odp) || odp->iop->rename == 0)
    return -1;
  return odp->iop->rename(odp, oname, ndp, nname);
}

int
fs_is_readonly(struct inode *ip)
{
  return !fs_writable(ip);
}

// For filesystems whose directories are not xv6 dirent arrays: present the
// entries as a stream of xv6 struct dirent, one entry per sizeof(dirent)
// bytes of offset, as user space (ls) expects.
int
vfs_dir_read(struct inode *dp, int user_dst, uint64 dst, uint64 off, uint n,
             int (*readdir)(struct inode*, int, char*, uint*))
{
  struct dirent de;
  char name[NAMEMAX];
  uint ino;
  int total = 0;

  if(off % sizeof(de))
    return 0;
  while(n - total >= sizeof(de)){
    int index = (off + total) / sizeof(de);
    int r = readdir(dp, index, name, &ino);
    if(r <= 0)
      break;
    memset(&de, 0, sizeof(de));
    de.inum = ino == 0 ? 1 : (ino > 0xffff ? 0xffff : ino);
    int len = strlen(name);
    memmove(de.name, name, len < DIRSIZ ? len : DIRSIZ);
    if(either_copyout(user_dst, dst + total, &de, sizeof(de)) < 0)
      return total ? total : -1;
    total += sizeof(de);
  }
  return total;
}
