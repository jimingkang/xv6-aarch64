#include "kernel/types.h"
#include "user/user.h"
#include "user/sync.h"

int
main(void)
{
  semaphore_t done;
  event_t start;
  katomic_t counter;
  int i, pid;

  if(sem_init(&done, 0) < 0 || event_init(&start, 0) < 0 ||
     katomic_init(&counter, 0) < 0){
    printf("syncdemo: create failed\n");
    exit(1);
  }

  for(i = 0; i < 3; i++){
    pid = fork();
    if(pid == 0){
      event_wait(&start);
      katomic_fetch_add(&counter, 1);
      sem_post(&done);
      exit(0);
    }
    if(pid < 0){
      printf("syncdemo: fork failed\n");
      exit(1);
    }
  }

  printf("syncdemo: notify children\n");
  event_notify(&start);
  for(i = 0; i < 3; i++)
    sem_wait(&done);
  printf("syncdemo: atomic counter=%d (expected 3)\n",
         katomic_load(&counter));
  for(i = 0; i < 3; i++)
    wait(0);

  event_reset(&start);
  event_destroy(&start);
  sem_destroy(&done);
  katomic_destroy(&counter);
  printf("syncdemo: ok\n");
  exit(0);
}
