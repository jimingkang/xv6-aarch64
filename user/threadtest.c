#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "user/sync.h"

static volatile int counter;
static int counter_lock;

static void
worker(void *arg)
{
  int loops = *(int *)arg;

  printf("threadtest: tid=%d tgid=%d cpu=%d started\n",
         gettid(), getpid(), getcpu());
  for(int i = 0; i < loops; i++){
    if(sync_wait(counter_lock) < 0)
      texit(2);
    counter++;
    if(sync_signal(counter_lock, 1) < 0)
      texit(3);
    if((i & 127) == 0)
      sched_yield();
  }
  texit(0);
}

int
main(void)
{
  thread_t a, b;
  int loops = 1000;
  int sa = -1, sb = -1;

  counter_lock = sync_create(SYNC_MUTEX, 1);
  if(counter_lock < 0){
    printf("threadtest: cannot create mutex\n");
    exit(1);
  }
  if(thread_create(&a, worker, &loops) < 0 ||
     thread_create(&b, worker, &loops) < 0){
    printf("threadtest: create failed\n");
    exit(1);
  }
  if(thread_join(&a, &sa) < 0 || thread_join(&b, &sb) < 0){
    printf("threadtest: join failed\n");
    exit(1);
  }
  sync_destroy(counter_lock);
  if(counter != 2 * loops || sa != 0 || sb != 0){
    printf("threadtest: FAIL counter=%d status=%d,%d\n", counter, sa, sb);
    exit(1);
  }
  printf("threadtest: PASS counter=%d tgid=%d\n", counter, getpid());
  exit(0);
}
