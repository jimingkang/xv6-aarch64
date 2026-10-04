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
  uint32 version;
  uint64 data;
  int queued;
};

struct epoll_ready {
  int slot;
  uint32 version;
};

struct epoll {
  struct spinlock lock;
  int used;
  uint generation;
  int ready_count;
  struct epoll_ready ready[EPOLL_MAX_WATCH];
  struct epoll_watch watch[EPOLL_MAX_WATCH];
};

static struct epoll epolls[NEPOLL];
static struct spinlock epoll_table_lock;

void
epollinit(void)
{
  initlock(&epoll_table_lock, "epoll table");
  for(int i = 0; i < NEPOLL; i++)
    initlock(&epolls[i].lock, "epoll");
}

static void
epoll_queue_locked(struct epoll *ep, int slot)
{
  struct epoll_watch *w = &ep->watch[slot];
  if(w->queued)
    return;
  if(ep->ready_count >= EPOLL_MAX_WATCH)
    panic("epoll ready queue");
  ep->ready[ep->ready_count].slot = slot;
  ep->ready[ep->ready_count].version = w->version;
  ep->ready_count++;
  w->queued = 1;
  ep->generation++;
  wakeup(ep);
}

static void
epoll_remove_queued_locked(struct epoll *ep, int slot)
{
  for(int i = 0; i < ep->ready_count; ){
    if(ep->ready[i].slot == slot){
      for(int j = i + 1; j < ep->ready_count; j++)
        ep->ready[j - 1] = ep->ready[j];
      ep->ready_count--;
    } else {
      i++;
    }
  }
  ep->watch[slot].queued = 0;
}

static void
epollnotify_source(int type, int socket, struct pty *pty, int pty_master)
{
  for(int i = 0; i < NEPOLL; i++){
    struct epoll *ep = &epolls[i];
    acquire(&ep->lock);
    if(ep->used){
      for(int j = 0; j < EPOLL_MAX_WATCH; j++){
        struct file *f = ep->watch[j].file;
        if(f == 0 || f->type != type)
          continue;
        if((type == FD_SOCKET && f->socket == socket) ||
           (type == FD_PTY && f->pty == pty &&
            f->pty_master == pty_master))
          epoll_queue_locked(ep, j);
      }
    }
    release(&ep->lock);
  }
}

void
epollnotify_socket(int handle)
{
  epollnotify_source(FD_SOCKET, handle, 0, 0);
}

void
epollnotify_pty(struct pty *pty, int master)
{
  epollnotify_source(FD_PTY, 0, pty, master);
}

struct epoll*
epollalloc(void)
{
  acquire(&epoll_table_lock);
  for(int i = 0; i < NEPOLL; i++){
    struct epoll *ep = &epolls[i];
    acquire(&ep->lock);
    if(!ep->used){
      ep->used = 1;
      ep->generation = 0;
      ep->ready_count = 0;
      memset(ep->ready, 0, sizeof(ep->ready));
      memset(ep->watch, 0, sizeof(ep->watch));
      release(&ep->lock);
      release(&epoll_table_lock);
      return ep;
    }
    release(&ep->lock);
  }
  release(&epoll_table_lock);
  return 0;
}

void
epollclose(struct epoll *ep)
{
  struct file *drop[EPOLL_MAX_WATCH];
  int n = 0;
  acquire(&epoll_table_lock);
  acquire(&ep->lock);
  for(int i = 0; i < EPOLL_MAX_WATCH; i++){
    if(ep->watch[i].file){
      drop[n++] = ep->watch[i].file;
      ep->watch[i].file = 0;
    }
  }
  ep->ready_count = 0;
  ep->used = 0;
  release(&ep->lock);
  release(&epoll_table_lock);
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
  if(!ep->used){
    release(&ep->lock);
    return -1;
  }
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
    struct epoll_watch *w = &ep->watch[free_slot];
    w->file = filedup(file);
    w->fd = fd;
    w->events = event->events;
    w->last_ready = 0;
    w->version++;
    if(w->version == 0)
      w->version++;
    w->data = event->data.u64;
    epoll_queue_locked(ep, free_slot);
  } else if(op == EPOLL_CTL_MOD){
    if(found < 0 || event == 0){
      release(&ep->lock);
      return -1;
    }
    struct epoll_watch *w = &ep->watch[found];
    epoll_remove_queued_locked(ep, found);
    w->events = event->events;
    w->last_ready = 0;
    w->version++;
    if(w->version == 0)
      w->version++;
    w->data = event->data.u64;
    epoll_queue_locked(ep, found);
  } else if(op == EPOLL_CTL_DEL){
    if(found < 0){
      release(&ep->lock);
      return -1;
    }
    epoll_remove_queued_locked(ep, found);
    drop = ep->watch[found].file;
    uint32 version = ep->watch[found].version + 1;
    if(version == 0)
      version++;
    memset(&ep->watch[found], 0, sizeof(ep->watch[found]));
    ep->watch[found].version = version;
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
    struct epoll_ready pending[EPOLL_MAX_WATCH];
    struct file *files[EPOLL_MAX_WATCH];
    uint32 events[EPOLL_MAX_WATCH];
    uint32 versions[EPOLL_MAX_WATCH];
    uint64 data[EPOLL_MAX_WATCH];
    int count = 0, n = 0, more_ready;

    acquire(&ep->lock);
    if(!ep->used){
      release(&ep->lock);
      return -1;
    }
    while(count < maxevents && ep->ready_count > 0){
      struct epoll_ready item = ep->ready[0];
      for(int i = 1; i < ep->ready_count; i++)
        ep->ready[i - 1] = ep->ready[i];
      ep->ready_count--;
      if(item.slot < 0 || item.slot >= EPOLL_MAX_WATCH)
        continue;
      struct epoll_watch *w = &ep->watch[item.slot];
      if(w->file == 0 || w->version != item.version || !w->queued)
        continue;
      w->queued = 0;
      pending[count] = item;
      files[count] = filedup(w->file);
      events[count] = w->events;
      versions[count] = w->version;
      data[count] = w->data;
      count++;
    }
    more_ready = ep->ready_count > 0;
    if(count == 0 && !more_ready && timeout_ms < 0){
      sleep(ep, &ep->lock);
      release(&ep->lock);
      continue;
    }
    release(&ep->lock);
    if(count == 0 && more_ready)
      continue;

    for(int i = 0; i < count; i++){
      int current = file_ready(files[i], events[i]);
      fileclose(files[i]);
      acquire(&ep->lock);
      struct epoll_watch *w = &ep->watch[pending[i].slot];
      if(ep->used && w->file && w->version == versions[i]){
        int occurred;
        if(events[i] & EPOLLET){
          occurred = current & ~w->last_ready;
          w->last_ready = current;
        } else {
          occurred = current;
        }
        if(occurred){
          ready[n].events = occurred;
          ready[n].pad = 0;
          ready[n].data.u64 = data[i];
          n++;
        }
        if(!(events[i] & EPOLLET) && current)
          epoll_queue_locked(ep, pending[i].slot);
      }
      release(&ep->lock);
    }
    if(n){
      if(copyout(myproc()->vm->pagetable, address, (char*)ready,
                 n * sizeof(ready[0])) < 0)
        return -1;
      return n;
    }
    if(timeout_ms == 0)
      return 0;
    if(myproc()->killed)
      return -1;
    if(timeout_ms >= 0){
      acquire(&tickslock);
      if(ticks - start >= timeout_ticks){
        release(&tickslock);
        return 0;
      }
      sleep(&ticks, &tickslock);
      release(&tickslock);
    }
  }
}
