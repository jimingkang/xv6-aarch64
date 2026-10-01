#ifndef XV6_WORKQUEUE_H
#define XV6_WORKQUEUE_H

#include "spinlock.h"

struct workqueue;

struct work_struct {
  void (*func)(struct work_struct *work);
  struct work_struct *next;
  int pending;
  int running;
  int rerun;
  struct workqueue *wq;
};

struct workqueue {
  struct spinlock lock;
  struct work_struct *head;
  struct work_struct *tail;
  char *name;
};

struct delayed_work {
  struct work_struct work;
  struct delayed_work *next;
  struct workqueue *target;
  uint64 expires;
  int delayed;
};

void workqueue_init(void);
void init_workqueue(struct workqueue *wq, char *name, int workers);
void init_work(struct work_struct *work,
               void (*func)(struct work_struct *work));
int queue_work(struct workqueue *wq, struct work_struct *work);
int schedule_work(struct work_struct *work);
void flush_work(struct work_struct *work);
void cancel_work_sync(struct work_struct *work);
void init_delayed_work(struct delayed_work *dwork,
                       void (*func)(struct work_struct *work));
int queue_delayed_work(struct workqueue *wq, struct delayed_work *dwork,
                       uint64 delay);
int mod_delayed_work(struct workqueue *wq, struct delayed_work *dwork,
                     uint64 delay);
void cancel_delayed_work_sync(struct delayed_work *dwork);
void workqueue_timer_tick(void);
uint64 workqueue_now(void);

#endif
