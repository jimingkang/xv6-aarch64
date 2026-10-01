#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "aarch64.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "workqueue.h"

struct workqueue {
  struct spinlock lock;
  struct work_struct *head;
  struct work_struct *tail;
};

static struct workqueue system_wq;

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
    wakeup(work);
    release(&wq->lock);
  }
}

void
workqueue_init(void)
{
  int pid;
  initlock(&system_wq.lock, "system_wq");
  system_wq.head = 0;
  system_wq.tail = 0;
  pid = kthread_create(kworker, &system_wq, "kworker");
  if(pid < 0)
    panic("workqueue kthread");
  printf("workqueue: system_wq kworker pid=%d\n", pid);
}

void
init_work(struct work_struct *work,
          void (*func)(struct work_struct *work))
{
  work->func = func;
  work->next = 0;
  work->pending = 0;
  work->running = 0;
}

int
schedule_work(struct work_struct *work)
{
  int queued = 0;

  if(work == 0 || work->func == 0)
    return 0;
  acquire(&system_wq.lock);
  if(!work->pending){
    work->pending = 1;
    work->next = 0;
    if(system_wq.tail)
      system_wq.tail->next = work;
    else
      system_wq.head = work;
    system_wq.tail = work;
    queued = 1;
    wakeup(&system_wq);
  }
  release(&system_wq.lock);
  return queued;
}

void
flush_work(struct work_struct *work)
{
  acquire(&system_wq.lock);
  while(work->pending || work->running)
    sleep(work, &system_wq.lock);
  release(&system_wq.lock);
}

void
cancel_work_sync(struct work_struct *work)
{
  struct work_struct *p, *prev = 0;

  acquire(&system_wq.lock);
  for(p = system_wq.head; p; prev = p, p = p->next){
    if(p != work)
      continue;
    if(prev)
      prev->next = p->next;
    else
      system_wq.head = p->next;
    if(system_wq.tail == p)
      system_wq.tail = prev;
    p->next = 0;
    p->pending = 0;
    break;
  }
  while(work->running)
    sleep(work, &system_wq.lock);
  release(&system_wq.lock);
}
