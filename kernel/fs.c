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

#define min(a, b) ((a) < (b) ? (a) : (b))
// One superblock per mounted xv6 filesystem, indexed by block device number:
// the root on dev 1 and the RAM-disk instances (rootfs, devtmpfs).
static struct superblock sbs[NBLKDEV];
void fs_mountinit(void);

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

static struct inode* iget(uint dev, uint inum);

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
static struct inode*
iget(uint dev, uint inum)
{
  struct inode *ip, *empty;

  acquire(&itable.lock);

  // Is the inode already in the table?
  empty = 0;
  for(ip = &itable.inode[0]; ip < &itable.inode[NINODE]; ip++){
    if(ip->ref > 0 && ip->dev == dev && ip->inum == inum){
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
  release(&itable.lock);

  return ip;
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
void
ilock(struct inode *ip)
{
  struct buf *bp;
  struct dinode *dip;

  if(ip == 0 || ip->ref < 1)
    panic("ilock");

  acquiresleep(&ip->lock);

  if(ip->valid == 0){
    bp = bread(ip->dev, IBLOCK(ip->inum, (*getsb(ip->dev))));
    dip = (struct dinode*)bp->data + ip->inum%IPB;
    ip->type = dip->type;
    ip->major = dip->major;
    ip->minor = dip->minor;
    ip->nlink = dip->nlink;
    ip->size = dip->size;
    memmove(ip->addrs, dip->addrs, sizeof(ip->addrs));
    brelse(bp);
    ip->valid = 1;
    if(ip->type == 0)
      panic("ilock: no type");
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
void
iput(struct inode *ip)
{
  acquire(&itable.lock);

  if(ip->ref == 1 && ip->valid && ip->nlink == 0){
    // inode has no links and no other references: truncate and free.

    // ip->ref == 1 means no other process can have ip locked,
    // so this acquiresleep() won't block (or deadlock).
    acquiresleep(&ip->lock);

    release(&itable.lock);

    itrunc(ip);
    ip->type = 0;
    iupdate(ip);
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
  if(len >= DIRSIZ)
    memmove(name, s, DIRSIZ);
  else {
    memmove(name, s, len);
    name[len] = 0;
  }
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
// ext2, FAT32, procfs and netfs are not xv6 inode filesystems; they stay in
// the path-prefix table in vfs.c, which the system calls consult first.

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
    ip = iget(dev, ROOTINO);
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
    if(ip->inum != ROOTINO)
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

// Is path at or below a native (xv6-inode) mount other than "/"?  With an
// ext2 root mounted on "/" in vfs.c, such paths (devtmpfs on /dev) must not
// be routed to ext2.
int
fs_native_covers(char *path)
{
  int r = 0;
  acquire(&mtab.lock);
  for(int i = 0; i < NFSMOUNT && !r; i++){
    struct fsmount *m = &mtab.m[i];
    int n = strlen(m->path);
    if(m->used && n > 1 && strncmp(path, m->path, n) == 0 &&
       (path[n] == 0 || path[n] == '/'))
      r = 1;
  }
  release(&mtab.lock);
  return r;
}

// Root directory of the xv6 filesystem on dev (referenced, unlocked).
struct inode*
fs_dev_root(int dev)
{
  return iget(dev, ROOTINO);
}

int
fs_is_mountpoint(struct inode *ip)
{
  acquire(&mtab.lock);
  int r = mount_covering(ip->dev, ip->inum) != 0 ||
          (ip->inum == ROOTINO && mount_of_dev(ip->dev) != 0);
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
  struct fsmount *m = from->inum == ROOTINO ? mount_of_dev(from->dev) : 0;
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
    p = mstr(p, e, " rw 0 0\n");
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
    if((next = dirlookup(ip, name, 0)) == 0){
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
  char name[DIRSIZ];
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
struct inode*
fs_create(struct inode *dp, char *name, short type, short major, short minor)
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
