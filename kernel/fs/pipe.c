#include <kernel/types.h>
#include "aarch64.h"
#include <kernel/defs.h>
#include <kernel/param.h>
#include <kernel/spinlock.h>
#include <kernel/proc.h>
#include <kernel/fs.h>
#include <kernel/sleeplock.h>
#include <kernel/file.h>
#include <kernel/device.h>
#include <kernel/epoll.h>

#define PIPESIZE 512

struct pipe {
  struct spinlock lock;
  char data[PIPESIZE];
  uint nread;     // number of bytes read
  uint nwrite;    // number of bytes written
  int readopen;   // read fd is still open
  int writeopen;  // write fd is still open
  struct file *readfile;
  struct file *writefile;
};

static int pipe_fread(struct file *f, int user_dst, uint64 addr, int n);
static int pipe_fwrite(struct file *f, int user_src, uint64 addr, int n);
static void pipe_release(struct file *f);
static int pipe_poll(struct file *f, int events);

static const struct file_operations pipe_file_ops = {
  .fread = pipe_fread,
  .fwrite = pipe_fwrite,
  .release = pipe_release,
  .poll = pipe_poll,
};

int
pipealloc(struct file **f0, struct file **f1)
{
  struct pipe *pi;

  pi = 0;
  *f0 = *f1 = 0;
  if((*f0 = filealloc()) == 0 || (*f1 = filealloc()) == 0)
    goto bad;
  if((pi = (struct pipe*)kalloc()) == 0)
    goto bad;
  pi->readopen = 1;
  pi->writeopen = 1;
  pi->nwrite = 0;
  pi->nread = 0;
  initlock(&pi->lock, "pipe");
  (*f0)->type = FD_PIPE;
  (*f0)->readable = 1;
  (*f0)->writable = 0;
  (*f0)->pipe = pi;
  (*f0)->f_op = &pipe_file_ops;
  (*f1)->type = FD_PIPE;
  (*f1)->readable = 0;
  (*f1)->writable = 1;
  (*f1)->pipe = pi;
  (*f1)->f_op = &pipe_file_ops;
  pi->readfile = *f0;
  pi->writefile = *f1;
  return 0;

 bad:
  if(pi)
    kfree((char*)pi);
  if(*f0)
    fileclose(*f0);
  if(*f1)
    fileclose(*f1);
  return -1;
}

void
pipeclose(struct pipe *pi, int writable)
{
  struct file *notify;
  acquire(&pi->lock);
  if(writable){
    pi->writeopen = 0;
    pi->writefile = 0;
    notify = pi->readfile;
    wakeup(&pi->nread);
  } else {
    pi->readopen = 0;
    pi->readfile = 0;
    notify = pi->writefile;
    wakeup(&pi->nwrite);
  }
  if(pi->readopen == 0 && pi->writeopen == 0){
    release(&pi->lock);
    kfree((char*)pi);
  } else {
    release(&pi->lock);
    if(notify)
      epollnotify_file(notify);
  }
}

int
pipewrite(struct pipe *pi, uint64 addr, int n)
{
  int i = 0;
  struct proc *pr = myproc();

  acquire(&pi->lock);
  while(i < n){
    if(pi->readopen == 0 || pr->killed){
      release(&pi->lock);
      return -1;
    }
    if(pi->nwrite == pi->nread + PIPESIZE){ //DOC: pipewrite-full
      wakeup(&pi->nread);
      sleep(&pi->nwrite, &pi->lock);
    } else {
      char ch;
      if(copyin(pr->vm->pagetable, &ch, addr + i, 1) == -1)
        break;
      pi->data[pi->nwrite++ % PIPESIZE] = ch;
      i++;
    }
  }
  wakeup(&pi->nread);
  release(&pi->lock);
  if(pi->readfile)
    epollnotify_file(pi->readfile);

  return i;
}

int
piperead(struct pipe *pi, uint64 addr, int n)
{
  int i;
  struct proc *pr = myproc();
  char ch;

  acquire(&pi->lock);
  while(pi->nread == pi->nwrite && pi->writeopen){  //DOC: pipe-empty
    if(pr->killed){
      release(&pi->lock);
      return -1;
    }
    sleep(&pi->nread, &pi->lock); //DOC: piperead-sleep
  }
  for(i = 0; i < n; i++){  //DOC: piperead-copy
    if(pi->nread == pi->nwrite)
      break;
    ch = pi->data[pi->nread++ % PIPESIZE];
    if(copyout(pr->vm->pagetable, addr + i, &ch, 1) == -1)
      break;
  }
  wakeup(&pi->nwrite);  //DOC: piperead-wakeup
  release(&pi->lock);
  if(pi->writefile)
    epollnotify_file(pi->writefile);
  return i;
}

static int
pipe_fread(struct file *f, int user_dst, uint64 addr, int n)
{
  if(!user_dst)
    return -1;
  return piperead(f->pipe, addr, n);
}

static int
pipe_fwrite(struct file *f, int user_src, uint64 addr, int n)
{
  if(!user_src)
    return -1;
  return pipewrite(f->pipe, addr, n);
}

static void
pipe_release(struct file *f)
{
  pipeclose(f->pipe, f->writable);
}

static int
pipe_poll(struct file *f, int events)
{
  struct pipe *pi = f->pipe;
  int ready = 0;
  acquire(&pi->lock);
  if(f->readable && (events & EPOLLIN) &&
     (pi->nread != pi->nwrite || !pi->writeopen))
    ready |= EPOLLIN;
  if(f->writable && (events & EPOLLOUT) && pi->readopen &&
     pi->nwrite != pi->nread + PIPESIZE)
    ready |= EPOLLOUT;
  if((f->readable && !pi->writeopen) || (f->writable && !pi->readopen))
    ready |= EPOLLERR;
  release(&pi->lock);
  return ready;
}
