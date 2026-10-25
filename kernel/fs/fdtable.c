// Per-task descriptor tables (files_struct) and filesystem contexts
// (fs_struct), shaped after Linux fs/file.c and fs/fs_struct.c.
//
// Both objects are reference counted so clone(CLONE_FILES) and
// clone(CLONE_FS) can share them between tasks, while fork() copies them.
// Each object lives in its own kalloc() page.

#include <kernel/types.h>
#include <kernel/param.h>
#include "aarch64.h"
#include <kernel/spinlock.h>
#include <kernel/sleeplock.h>
#include <kernel/fs.h>
#include <kernel/file.h>
#include <kernel/proc.h>
#include <kernel/defs.h>

struct files_struct*
files_alloc(void)
{
  struct files_struct *files = kalloc();

  if(files == 0)
    return 0;
  memset(files, 0, sizeof(*files));
  initlock(&files->lock, "files");
  files->count = 1;
  return files;
}

// Private copy for fork() and exec(): every open descriptor is duplicated,
// so the copies reference the same open-file descriptions.
struct files_struct*
files_dup(struct files_struct *old)
{
  struct files_struct *files = files_alloc();

  if(files == 0)
    return 0;
  acquire(&old->lock);
  for(int fd = 0; fd < NOFILE; fd++)
    if(old->fd[fd])
      files->fd[fd] = filedup(old->fd[fd]);
  release(&old->lock);
  return files;
}

void
files_get(struct files_struct *files)
{
  acquire(&files->lock);
  files->count++;
  release(&files->lock);
}

// Drop one task's reference.  The last one closes every descriptor, which
// may sleep, so callers must not hold spinlocks unless the table is empty.
void
files_put(struct files_struct *files)
{
  int last;

  if(files == 0)
    return;
  acquire(&files->lock);
  if(files->count < 1)
    panic("files_put");
  last = --files->count == 0;
  release(&files->lock);
  if(!last)
    return;
  for(int fd = 0; fd < NOFILE; fd++){
    if(files->fd[fd]){
      fileclose(files->fd[fd]);
      files->fd[fd] = 0;
    }
  }
  kfree(files);
}

// exec() gives the new image a private table, as Linux unshare_files().
int
unshare_files(void)
{
  struct proc *p = myproc();
  struct files_struct *old = p->files, *files;
  int shared;

  acquire(&old->lock);
  shared = old->count > 1;
  release(&old->lock);
  if(!shared)
    return 0;
  if((files = files_dup(old)) == 0)
    return -1;
  p->files = files;
  files_put(old);
  return 0;
}

// Install f in the lowest free descriptor.  Takes over the caller's file
// reference on success.
int
fd_install(struct file *f)
{
  struct files_struct *files = myproc()->files;

  acquire(&files->lock);
  for(int fd = 0; fd < NOFILE; fd++){
    if(files->fd[fd] == 0){
      files->fd[fd] = f;
      release(&files->lock);
      return fd;
    }
  }
  release(&files->lock);
  return -1;
}

// Store f (possibly 0) at fd and return the previous entry, whose reference
// now belongs to the caller.
struct file*
fd_replace(int fd, struct file *f)
{
  struct files_struct *files = myproc()->files;
  struct file *old;

  if(fd < 0 || fd >= NOFILE)
    return 0;
  acquire(&files->lock);
  old = files->fd[fd];
  files->fd[fd] = f;
  release(&files->lock);
  return old;
}

// Linux fget(): a new reference to the file at fd, or 0.  The caller drops
// it with fileclose().
struct file*
fget(int fd)
{
  struct files_struct *files = myproc()->files;
  struct file *f = 0;

  if(fd < 0 || fd >= NOFILE)
    return 0;
  acquire(&files->lock);
  if(files->fd[fd])
    f = filedup(files->fd[fd]);
  release(&files->lock);
  return f;
}

// Linux fdget(): the file at fd, valid until the current system call
// returns.  A private table cannot change under us, so only a shared one
// pins a reference; fdput_all() releases the pins after the handler.
struct file*
fdget(int fd)
{
  struct proc *p = myproc();
  struct files_struct *files = p->files;
  struct file *f;

  if(fd < 0 || fd >= NOFILE)
    return 0;
  acquire(&files->lock);
  f = files->fd[fd];
  if(f && files->count > 1){
    if(p->nfpin >= NFPIN)
      f = 0;
    else
      p->fpin[p->nfpin++] = filedup(f);
  }
  release(&files->lock);
  return f;
}

void
fdput_all(struct proc *p)
{
  while(p->nfpin > 0){
    struct file *f = p->fpin[--p->nfpin];
    p->fpin[p->nfpin] = 0;
    fileclose(f);
  }
}

struct fs_struct*
fs_alloc(void)
{
  struct fs_struct *fs = kalloc();

  if(fs == 0)
    return 0;
  memset(fs, 0, sizeof(*fs));
  initlock(&fs->lock, "fs_struct");
  fs->users = 1;
  safestrcpy(fs->cwdpath, "/", sizeof(fs->cwdpath));
  return fs;
}

struct fs_struct*
fs_copy(struct fs_struct *old)
{
  struct fs_struct *fs = fs_alloc();

  if(fs == 0)
    return 0;
  acquire(&old->lock);
  if(old->cwd)
    fs->cwd = idup(old->cwd);
  safestrcpy(fs->cwdpath, old->cwdpath, sizeof(fs->cwdpath));
  release(&old->lock);
  return fs;
}

void
fs_get(struct fs_struct *fs)
{
  acquire(&fs->lock);
  fs->users++;
  release(&fs->lock);
}

// Drop one task's reference.  The last one releases cwd, which may sleep,
// so callers must not hold spinlocks unless cwd is already 0.
void
fs_put(struct fs_struct *fs)
{
  int last;

  if(fs == 0)
    return;
  acquire(&fs->lock);
  if(fs->users < 1)
    panic("fs_put");
  last = --fs->users == 0;
  release(&fs->lock);
  if(!last)
    return;
  if(fs->cwd){
    begin_op();
    iput(fs->cwd);
    end_op();
  }
  kfree(fs);
}

// A new reference to the caller's current directory.
struct inode*
fs_cwd(void)
{
  struct fs_struct *fs = myproc()->fs;
  struct inode *ip;

  acquire(&fs->lock);
  ip = fs->cwd ? idup(fs->cwd) : 0;
  release(&fs->lock);
  return ip;
}

// Install ip (whose reference passes to the fs_struct) and path as the
// current directory; return the previous cwd for the caller to iput().
struct inode*
fs_set_cwd(struct inode *ip, char *path)
{
  struct fs_struct *fs = myproc()->fs;
  struct inode *old;

  acquire(&fs->lock);
  old = fs->cwd;
  fs->cwd = ip;
  if(path)
    safestrcpy(fs->cwdpath, path, sizeof(fs->cwdpath));
  release(&fs->lock);
  return old;
}

void
fs_cwdpath(char *buf, int size)
{
  struct fs_struct *fs = myproc()->fs;

  acquire(&fs->lock);
  safestrcpy(buf, fs->cwdpath, size);
  release(&fs->lock);
}
