#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "aarch64.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "workqueue.h"

static struct workqueue system_wq;
#define SYSTEM_WQ_WORKERS NCPU
static struct spinlock delayed_lock;
static struct delayed_work *delayed_head;
static uint64 work_jiffies;

static void
kworker(void *arg)
{
  struct workqueue *wq = arg;

  for(;;){
    acquire(&wq->lock);
    while(wq->head == 0)
      sleep(wq, &wq->lock);

    struct work_struct *work = wq->head;
    wq->head = work->next;
    if(wq->head == 0)
      wq->tail = 0;
    work->next = 0;
    work->pending = 0;
    work->running = 1;
    release(&wq->lock);

    work->func(work);

    acquire(&wq->lock);
    work->running = 0;
    if(work->rerun){
      work->rerun = 0;
      work->pending = 1;
      work->next = 0;
      if(wq->tail)
        wq->tail->next = work;
      else
        wq->head = work;
      wq->tail = work;
      wakeup(wq);
    }
    wakeup(work);
    release(&wq->lock);
  }
}

void
init_workqueue(struct workqueue *wq, char *name, int workers)
{
  int pid;
  initlock(&wq->lock, name);
  wq->head = 0;
  wq->tail = 0;
  wq->name = name;
  for(int i = 0; i < workers; i++){
    pid = kthread_create(kworker, wq, name);
    if(pid < 0)
      panic("workqueue kthread");
    printf("workqueue: %s worker=%d pid=%d\n", name, i, pid);
  }
}

void
workqueue_init(void)
{
  initlock(&delayed_lock, "delayed work");
  delayed_head = 0;
  work_jiffies = 0;
  init_workqueue(&system_wq, "system_wq", SYSTEM_WQ_WORKERS);
}

void
init_work(struct work_struct *work,
          void (*func)(struct work_struct *work))
{
  work->func = func;
  work->next = 0;
  work->pending = 0;
  work->running = 0;
  work->rerun = 0;
  work->wq = 0;
}

int
queue_work(struct workqueue *wq, struct work_struct *work)
{
  int queued = 0;

  if(work == 0 || work->func == 0)
    return 0;
  if(wq == 0)
    return 0;
  acquire(&wq->lock);
  if(work->running){
    // Coalesce any number of IRQ/timer arrivals while this callback runs.
    // It will be queued once more after returning, never run concurrently.
    work->rerun = 1;
  } else if(!work->pending){
    work->wq = wq;
    work->pending = 1;
    work->next = 0;
    if(wq->tail)
      wq->tail->next = work;
    else
      wq->head = work;
    wq->tail = work;
    queued = 1;
    wakeup(wq);
  }
  release(&wq->lock);
  return queued;
}

int
schedule_work(struct work_struct *work)
{
  return queue_work(&system_wq, work);
}

void
flush_work(struct work_struct *work)
{
  struct workqueue *wq = work->wq ? work->wq : &system_wq;
  acquire(&wq->lock);
  while(work->pending || work->running)
    sleep(work, &wq->lock);
  release(&wq->lock);
}

void
cancel_work_sync(struct work_struct *work)
{
  struct work_struct *p, *prev = 0;
  struct workqueue *wq = work->wq ? work->wq : &system_wq;

  acquire(&wq->lock);
  for(p = wq->head; p; prev = p, p = p->next){
    if(p != work)
      continue;
    if(prev)
      prev->next = p->next;
    else
      wq->head = p->next;
    if(wq->tail == p)
      wq->tail = prev;
    p->next = 0;
    p->pending = 0;
    break;
  }
  work->rerun = 0;
  while(work->running)
    sleep(work, &wq->lock);
  release(&wq->lock);
}

void
init_delayed_work(struct delayed_work *dwork,
                  void (*func)(struct work_struct *work))
{
  init_work(&dwork->work, func);
  dwork->next = 0;
  dwork->target = 0;
  dwork->expires = 0;
  dwork->delayed = 0;
}

static void
delayed_remove_locked(struct delayed_work *dwork)
{
  struct delayed_work **p;
  for(p = &delayed_head; *p; p = &(*p)->next)
    if(*p == dwork){
      *p = dwork->next;
      dwork->next = 0;
      dwork->delayed = 0;
      return;
    }
}

static void
delayed_insert_locked(struct delayed_work *dwork)
{
  struct delayed_work **p = &delayed_head;
  while(*p && (long)((*p)->expires - dwork->expires) <= 0)
    p = &(*p)->next;
  dwork->next = *p;
  *p = dwork;
  dwork->delayed = 1;
}

int
queue_delayed_work(struct workqueue *wq, struct delayed_work *dwork,
                   uint64 delay)
{
  if(wq == 0 || dwork == 0 || dwork->work.func == 0)
    return 0;
  acquire(&delayed_lock);
  if(dwork->delayed){
    release(&delayed_lock);
    return 0;
  }
  dwork->target = wq;
  dwork->expires = work_jiffies + (delay ? delay : 1);
  delayed_insert_locked(dwork);
  release(&delayed_lock);
  return 1;
}

int
mod_delayed_work(struct workqueue *wq, struct delayed_work *dwork,
                 uint64 delay)
{
  if(wq == 0 || dwork == 0 || dwork->work.func == 0)
    return 0;
  acquire(&delayed_lock);
  if(dwork->delayed)
    delayed_remove_locked(dwork);
  dwork->target = wq;
  dwork->expires = work_jiffies + (delay ? delay : 1);
  delayed_insert_locked(dwork);
  release(&delayed_lock);
  return 1;
}

void
cancel_delayed_work_sync(struct delayed_work *dwork)
{
  acquire(&delayed_lock);
  if(dwork->delayed)
    delayed_remove_locked(dwork);
  release(&delayed_lock);
  cancel_work_sync(&dwork->work);
}

void
workqueue_timer_tick(void)
{
  struct delayed_work *due = 0, **tail = &due;

  acquire(&delayed_lock);
  work_jiffies++;
  while(delayed_head && (long)(work_jiffies - delayed_head->expires) >= 0){
    struct delayed_work *dwork = delayed_head;
    delayed_head = dwork->next;
    dwork->next = 0;
    dwork->delayed = 0;
    *tail = dwork;
    tail = &dwork->next;
  }
  release(&delayed_lock);
  while(due){
    struct delayed_work *next = due->next;
    due->next = 0;
    queue_work(due->target, &due->work);
    due = next;
  }
}

uint64
workqueue_now(void)
{
  uint64 now;
  acquire(&delayed_lock);
  now = work_jiffies;
  release(&delayed_lock);
  return now;
}
