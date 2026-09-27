#include "kernel/types.h"
#include "user/user.h"
#include "user/sync.h"

#define NBUF 4
#define NITEM 10

struct shared_queue {
  semaphore_t empty;
  semaphore_t full;
  mutex_t lock;
  katomic_t put;
  katomic_t get;
  katomic_t slot[NBUF];
};

static int
queue_init(struct shared_queue *q)
{
  int i;

  if(sem_init(&q->empty, NBUF) < 0 || sem_init(&q->full, 0) < 0 ||
     mutex_init(&q->lock) < 0 || katomic_init(&q->put, 0) < 0 ||
     katomic_init(&q->get, 0) < 0)
    return -1;
  for(i = 0; i < NBUF; i++)
    if(katomic_init(&q->slot[i], 0) < 0)
      return -1;
  return 0;
}

static void
queue_destroy(struct shared_queue *q)
{
  int i;
  sem_destroy(&q->empty);
  sem_destroy(&q->full);
  mutex_destroy(&q->lock);
  katomic_destroy(&q->put);
  katomic_destroy(&q->get);
  for(i = 0; i < NBUF; i++)
    katomic_destroy(&q->slot[i]);
}

static void
produce(struct shared_queue *q, int value)
{
  int pos;

  sem_wait(&q->empty);       // wait until the bounded buffer has space
  mutex_lock(&q->lock);
  pos = katomic_load(&q->put);
  katomic_store(&q->slot[pos], value);
  katomic_store(&q->put, (pos + 1) % NBUF);
  mutex_unlock(&q->lock);
  sem_post(&q->full);        // publish one complete item
}

static int
consume(struct shared_queue *q)
{
  int pos, value;

  sem_wait(&q->full);        // wait until an item is available
  mutex_lock(&q->lock);
  pos = katomic_load(&q->get);
  value = katomic_load(&q->slot[pos]);
  katomic_store(&q->get, (pos + 1) % NBUF);
  mutex_unlock(&q->lock);
  sem_post(&q->empty);       // return one slot to the producer
  return value;
}

int
main(void)
{
  struct shared_queue q;
  int i, pid;

  if(queue_init(&q) < 0){
    printf("prodcons: initialization failed\n");
    exit(1);
  }

  pid = fork();
  if(pid < 0){
    printf("prodcons: fork failed\n");
    exit(1);
  }
  if(pid == 0){
    for(i = 1; i <= NITEM; i++){
      printf("producer: %d\n", i);
      produce(&q, i);
    }
    exit(0);
  }

  for(i = 0; i < NITEM; i++)
    printf("consumer: %d\n", consume(&q));
  wait(0);
  queue_destroy(&q);
  printf("prodcons: ok\n");
  exit(0);
}
