#ifndef XV6_WORKQUEUE_H
#define XV6_WORKQUEUE_H

struct work_struct {
  void (*func)(struct work_struct *work);
  struct work_struct *next;
  int pending;
  int running;
};

void workqueue_init(void);
void init_work(struct work_struct *work,
               void (*func)(struct work_struct *work));
int schedule_work(struct work_struct *work);
void flush_work(struct work_struct *work);
void cancel_work_sync(struct work_struct *work);

#endif
