#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

struct thread_start {
  void (*fn)(void *);
  void *arg;
};

static void
thread_boot(void *opaque)
{
  struct thread_start start = *(struct thread_start *)opaque;

  // The creator no longer needs this small launch record.  The separately
  // allocated stack remains owned by thread_t until thread_join().
  free(opaque);
  start.fn(start.arg);
  texit(0);
}

int
thread_create(thread_t *thread, void (*fn)(void *), void *arg)
{
  struct thread_start *start;
  uint64 top;

  if(thread == 0 || fn == 0)
    return -1;
  thread->tid = -1;
  thread->stack = malloc(THREAD_STACK_SIZE + 16);
  if(thread->stack == 0)
    return -1;
  start = malloc(sizeof(*start));
  if(start == 0){
    free(thread->stack);
    thread->stack = 0;
    return -1;
  }
  start->fn = fn;
  start->arg = arg;
  top = ((uint64)thread->stack + THREAD_STACK_SIZE + 16) & ~15ULL;
  thread->tid = clone(thread_boot, start, (void *)top);
  if(thread->tid < 0){
    free(start);
    free(thread->stack);
    thread->stack = 0;
    return -1;
  }
  return 0;
}

int
thread_join(thread_t *thread, int *status)
{
  int tid;

  if(thread == 0 || thread->tid < 0)
    return -1;
  tid = tjoin(thread->tid, status);
  if(tid < 0)
    return -1;
  free(thread->stack);
  thread->stack = 0;
  thread->tid = -1;
  return tid;
}
