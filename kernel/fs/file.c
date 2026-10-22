//
// Support functions for system calls that involve file descriptors.
//

#include <kernel/types.h>
#include "aarch64.h"
#include <kernel/defs.h>
#include <kernel/param.h>
#include <kernel/fs.h>
#include <kernel/spinlock.h>
#include <kernel/sleeplock.h>
#include <kernel/file.h>
#include <kernel/vfs.h>
#include <kernel/stat.h>
#include <kernel/proc.h>
#include <kernel/device.h>
#include <kernel/epoll.h>
#include <kernel/fcntl.h>
#include <kernel/fs_trace.h>

struct {
  struct spinlock lock;
  struct file file[NFILE];
} ftable;

// Trace only the first operations after boot.  This diagnostic counter is
// deliberately approximate on SMP: a race may print one extra line, but it
// avoids pulling libgcc atomic helpers into the freestanding kernel.
#if FS_TRACE
static int vfs_io_trace_budget = 0x7fffffff;

static int
vfs_io_trace_take(void)
{
  if(vfs_io_trace_budget <= 0)
    return 0;
  vfs_io_trace_budget--;
  return 1;
}
#endif

static void
vfs_io_trace(char *op, struct inode *ip, uint64 off, int requested, int result)
{
  struct super_block *sb = getsuper(ip->dev);
  printf("fs-trace: file_%s: inode_ops->%s fs=%s dev=%d ino=%d off=%p requested=%d result=%d\n",
         op, op, sb ? sb->type : "unknown", ip->dev, ip->inum, off,
         requested, result);
}

void
fileinit(void)
{
  initlock(&ftable.lock, "ftable");
  epollinit();
}

// Allocate a file structure.
struct file*
filealloc(void)
{
  struct file *f;

  acquire(&ftable.lock);
  for(f = ftable.file; f < ftable.file + NFILE; f++){
    if(f->ref == 0){
      memset(f, 0, sizeof(*f));
      f->ref = 1;
      mutex_init(&f->pos_lock, "file-position");
      release(&ftable.lock);
      return f;
    }
  }
  release(&ftable.lock);
  return 0;
}

// Increment ref count for file f.
struct file*
filedup(struct file *f)
{
  acquire(&ftable.lock);
  if(f->ref < 1)
    panic("filedup");
  f->ref++;
  release(&ftable.lock);
  return f;
}

// Close file f.  (Decrement ref count, close when reaches 0.)
void
fileclose(struct file *f)
{
  struct file ff;

  acquire(&ftable.lock);
  if(f->ref < 1)
    panic("fileclose");
  if(--f->ref > 0){
    release(&ftable.lock);
    return;
  }
  ff = *f;
  f->ref = 0;
  f->type = FD_NONE;
  release(&ftable.lock);

  if(ff.f_op && ff.f_op->release)
    ff.f_op->release(&ff);

  // Inodes are references owned by both regular and device file objects.
  // Their common lifetime is intentionally outside the operations table:
  // drivers release private state, then the file core drops the VFS inode.
  if(ff.ip){
    // Releasing a completed sdroot updater must not start a transaction on
    // the now-replaced root image.  Keep this one in-memory inode reference
    // until the mandatory reboot instead of touching stale root metadata.
    if(ff.type == FD_DEVICE && ff.major == ROOTUPDATE &&
       rootupdate_root_frozen())
      return;
    begin_op();
    iput(ff.ip);
    end_op();
  }
}

// Get metadata about file f.
// addr is a user virtual address, pointing to a struct stat.
int
filestat(struct file *f, uint64 addr)
{
  struct proc *p = myproc();
  struct stat st;
  
  if(f->type == FD_INODE || f->type == FD_DEVICE){
    ireadlock(f->ip);
    stati(f->ip, &st);
    FSTRACE("filestat: fd file -> dev=%d ino=%d type=%d nlink=%d size=%p\n",
            st.dev, st.ino, st.type, st.nlink, st.size);
    ireadunlock(f->ip);
    if(copyout(p->vm->pagetable, addr, (char *)&st, sizeof(st)) < 0)
      return -1;
    return 0;
  }
  return -1;
}

// Read from file f.
// addr is a user virtual address.
int
fileread(struct file *f, uint64 addr, int n)
{
  int r = 0;
  uint64 oldoff = f->off;
  int trace = 0;
#if FS_TRACE
  trace = f->type == FD_INODE && vfs_io_trace_take();
#endif

  if(f->readable == 0 || f->f_op == 0)
    return -1;
  if(f->f_op->fread)
    r = f->f_op->fread(f, 1, addr, n);
  else if(f->f_op->read)
    r = f->f_op->read(1, addr, n);
  else
    return -1;

  if(trace)
    vfs_io_trace("read", f->ip, oldoff, n, r);

  return r;
}

// Write n bytes from user address addr at offset off through the inode's
// filesystem; returns the bytes written.  xv6fs writes go through the log a
// few blocks per transaction (inode, indirect block, allocation blocks and
// 2 blocks of slop for non-aligned writes must fit in one transaction);
// other filesystems take the whole request.
static int
inode_write(struct inode *ip, uint64 addr, uint64 off, int n)
{
  struct super_block *sb = getsuper(ip->dev);
  int max = (sb && sb->logged) ? ((MAXOPBLOCKS-1-1-2) / 2) * BSIZE : n;
  int i = 0;

  if(ip->iop->write == 0)
    return -1;
  while(i < n){
    int n1 = n - i;
    if(n1 > max)
      n1 = max;
    begin_op();
    ilock(ip);
    int r = ip->iop->write(ip, 1, addr + i, off + i, n1);
    iunlock(ip);
    end_op();
    if(r <= 0)
      break;
    i += r;
    if(r != n1)
      break;
  }
  return i;
}

// Write to file f.
// addr is a user virtual address.
int
filewrite(struct file *f, uint64 addr, int n)
{
  int ret = 0;
  uint64 oldoff = f->off;
  int trace = 0;
#if FS_TRACE
  trace = f->type == FD_INODE && vfs_io_trace_take();
#endif

  if(f->writable == 0 || f->f_op == 0)
    return -1;
  if(f->f_op->fwrite)
    ret = f->f_op->fwrite(f, 1, addr, n);
  else if(f->f_op->write)
    ret = f->f_op->write(1, addr, n);
  else
    return -1;

  if(trace)
    vfs_io_trace("write", f->ip, oldoff, n, ret);

  return ret;
}

static int
inode_fread(struct file *f, int user_dst, uint64 addr, int n)
{
  int r;
  mutex_lock(&f->pos_lock);
  ireadlock(f->ip);
  r = f->ip->iop->read(f->ip, user_dst, addr, f->off, n);
  if(r > 0)
    f->off += r;
  ireadunlock(f->ip);
  mutex_unlock(&f->pos_lock);
  return r;
}

static int
inode_fwrite(struct file *f, int user_src, uint64 addr, int n)
{
  if(!user_src)
    return -1;
  mutex_lock(&f->pos_lock);
  int done = inode_write(f->ip, addr, f->off, n);
  if(done > 0)
    f->off += done;
  mutex_unlock(&f->pos_lock);
  return done == n ? n : -1;
}

static int
inode_poll(struct file *f, int events)
{
  int ready = 0;
  if(f->readable && (events & EPOLLIN))
    ready |= EPOLLIN;       // EOF is readable and returns zero bytes
  if(f->writable && (events & EPOLLOUT))
    ready |= EPOLLOUT;
  return ready;
}

static const struct file_operations inode_file_ops = {
  .fread = inode_fread,
  .fwrite = inode_fwrite,
  .poll = inode_poll,
};

void
file_inode_setup(struct file *f)
{
  f->f_op = &inode_file_ops;
}

int
filepoll(struct file *f, int events)
{
  return f && f->f_op && f->f_op->poll ? f->f_op->poll(f, events) : 0;
}

int
fileioctl(struct file *f, uint64 cmd, uint64 arg)
{
  return f && f->f_op && f->f_op->ioctl ? f->f_op->ioctl(f, cmd, arg) : -1;
}

uint64
filemmap(struct file *f, uint64 addr, uint64 len, int prot, int flags,
         uint64 off)
{
  return f && f->f_op && f->f_op->mmap ?
    f->f_op->mmap(f, addr, len, prot, flags, off) : (uint64)-1;
}

int
filemunmap(struct file *f, uint64 addr, uint64 len)
{
  return f && f->f_op && f->f_op->munmap ?
    f->f_op->munmap(f, addr, len) : -1;
}

int
filepread(struct file *f, uint64 addr, int n, uint64 off)
{
  int r;
  if(n < 0 || !f->readable || f->type != FD_INODE)
    return -1;
  ireadlock(f->ip);
  r = f->ip->iop->read(f->ip, 1, addr, off, n);
  ireadunlock(f->ip);
  return r;
}

int
filepwrite(struct file *f, uint64 addr, int n, uint64 off)
{
  if(n < 0 || !f->writable || f->type != FD_INODE)
    return -1;
  int done = inode_write(f->ip, addr, off, n);
  return done == n ? n : -1;
}

int
filetruncate(struct file *f, uint64 size)
{
  int r = -1;
  if(!f->writable || f->type != FD_INODE)
    return -1;
  begin_op();
  ilock(f->ip);
  if(f->ip->iop->truncate)
    r = f->ip->iop->truncate(f->ip, size);
  iunlock(f->ip);
  end_op();
  return r;
}

int
filefsync(struct file *f, int dataonly)
{
  int r = 0;
  (void)dataonly;
  if(f->type != FD_INODE)
    return -1;
  ilock(f->ip);
  if(f->ip->iop->fsync)
    r = f->ip->iop->fsync(f->ip);
  iunlock(f->ip);
  return r;
}
