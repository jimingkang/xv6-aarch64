#include "types.h"
#include "param.h"
#include "aarch64.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "epoll.h"

struct epoll_watch {
  struct file *file;
  int fd;
  uint32 events;
  uint32 last_ready;
  uint64 data;
};

struct epoll {
  struct spinlock lock;
  int used;
  struct epoll_watch watch[EPOLL_MAX_WATCH];
};

static struct epoll epolls[NEPOLL];
static struct spinlock epoll_table_lock;
static struct spinlock epoll_wait_lock;
static uint epoll_generation;

void
epollinit(void)
{
  initlock(&epoll_table_lock, "epoll table");
  initlock(&epoll_wait_lock, "epoll wait");
  for(int i = 0; i < NEPOLL; i++)
    initlock(&epolls[i].lock, "epoll");
}

void
epollnotify(void)
{
  acquire(&epoll_wait_lock);
  epoll_generation++;
  wakeup(&epoll_generation);
  release(&epoll_wait_lock);
}

struct epoll*
epollalloc(void)
{
  acquire(&epoll_table_lock);
  for(int i = 0; i < NEPOLL; i++){
    if(!epolls[i].used){
      epolls[i].used = 1;
      memset(epolls[i].watch, 0, sizeof(epolls[i].watch));
      release(&epoll_table_lock);
      return &epolls[i];
    }
  }
  release(&epoll_table_lock);
  return 0;
}

void
epollclose(struct epoll *ep)
{
  struct file *drop[EPOLL_MAX_WATCH];
  int n = 0;
  acquire(&ep->lock);
  for(int i = 0; i < EPOLL_MAX_WATCH; i++){
    if(ep->watch[i].file){
      drop[n++] = ep->watch[i].file;
      ep->watch[i].file = 0;
    }
  }
  ep->used = 0;
  release(&ep->lock);
  while(n)
    fileclose(drop[--n]);
}

int
epollctl(struct epoll *ep, int op, int fd, struct file *file,
         struct epoll_event *event)
{
  int free_slot = -1, found = -1;
  struct file *drop = 0;
  acquire(&ep->lock);
  for(int i = 0; i < EPOLL_MAX_WATCH; i++){
    if(ep->watch[i].file && ep->watch[i].fd == fd)
      found = i;
    if(ep->watch[i].file == 0 && free_slot < 0)
      free_slot = i;
  }
  if(op == EPOLL_CTL_ADD){
    if(found >= 0 || free_slot < 0 || file == 0 || event == 0){
      release(&ep->lock);
      return -1;
    }
    ep->watch[free_slot].file = filedup(file);
    ep->watch[free_slot].fd = fd;
    ep->watch[free_slot].events = event->events;
    ep->watch[free_slot].last_ready = 0;
    ep->watch[free_slot].data = event->data.u64;
  } else if(op == EPOLL_CTL_MOD){
    if(found < 0 || event == 0){
      release(&ep->lock);
      return -1;
    }
    ep->watch[found].events = event->events;
    ep->watch[found].last_ready = 0;
    ep->watch[found].data = event->data.u64;
  } else if(op == EPOLL_CTL_DEL){
    if(found < 0){
      release(&ep->lock);
      return -1;
    }
    drop = ep->watch[found].file;
    memset(&ep->watch[found], 0, sizeof(ep->watch[found]));
  } else {
    release(&ep->lock);
    return -1;
  }
  release(&ep->lock);
  if(drop)
    fileclose(drop);
  return 0;
}

static int
file_ready(struct file *f, int events)
{
  if(f->type == FD_SOCKET)
    return net_tcp_poll(f->socket, events);
  if(f->type == FD_PTY)
    return ptypoll(f->pty, f->pty_master, events);
  return 0;
}

int
epollwait(struct epoll *ep, uint64 address, int maxevents, int timeout_ms)
{
  struct epoll_event ready[EPOLL_MAX_WATCH];
  uint start, timeout_ticks;
  if(maxevents <= 0 || maxevents > EPOLL_MAX_WATCH || timeout_ms < -1)
    return -1;
  acquire(&tickslock);
  start = ticks;
  release(&tickslock);
  timeout_ticks = timeout_ms < 0 ? 0 : (timeout_ms + 99) / 100;
  for(;;){
    uint generation;
    acquire(&epoll_wait_lock);
    generation = epoll_generation;
    release(&epoll_wait_lock);
    int n = 0;
    acquire(&ep->lock);
    for(int i = 0; i < EPOLL_MAX_WATCH && n < maxevents; i++){
      if(ep->watch[i].file){
        int current = file_ready(ep->watch[i].file, ep->watch[i].events);
        int events;
        if(ep->watch[i].events & EPOLLET){
          events = current & ~ep->watch[i].last_ready;
          ep->watch[i].last_ready = current;
        } else {
          events = current;
        }
        if(events){
          ready[n].events = events;
          ready[n].pad = 0;
          ready[n].data.u64 = ep->watch[i].data;
          n++;
        }
      }
    }
    release(&ep->lock);
    if(n){
      if(copyout(myproc()->pagetable, address, (char*)ready,
                 n * sizeof(ready[0])) < 0)
        return -1;
      return n;
    }
    if(timeout_ms == 0)
      return 0;
    if(timeout_ms < 0){
      acquire(&epoll_wait_lock);
      if(epoll_generation == generation)
        sleep(&epoll_generation, &epoll_wait_lock);
      release(&epoll_wait_lock);
      continue;
    }
    acquire(&tickslock);
    if((timeout_ms >= 0 && ticks - start >= timeout_ticks) ||
       myproc()->killed){
      release(&tickslock);
      return myproc()->killed ? -1 : 0;
    }
    sleep(&ticks, &tickslock);
    release(&tickslock);
  }
}
