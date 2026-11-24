// mjvm platform layer for a POSIX host (testing only).
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

void *
plat_chunk(long size)
{
  void *p = mmap(0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return p == MAP_FAILED ? 0 : p;
}

int
plat_read_file(const char *path, char **data, int *len)
{
  FILE *f = fopen(path, "rb");
  if(!f) return -1;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  *data = malloc(n + 16);
  *len = (int)fread(*data, 1, n, f);
  fclose(f);
  return 0;
}

void plat_write(const char *s, int n) { if(write(1, s, n) < 0) {} }
void plat_exit(int code) { exit(code); }

struct start { void (*fn)(void *); void *arg; };
static void *tramp(void *p) { struct start s = *(struct start *)p; free(p); s.fn(s.arg); return 0; }

long
plat_thread_start(void (*fn)(void *), void *arg, char *stack, long stacksize)
{
  pthread_t th;
  pthread_attr_t at;
  struct start *s = malloc(sizeof *s);
  s->fn = fn; s->arg = arg;
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, stacksize);
  if(pthread_create(&th, &at, tramp, s)) return -1;
  return (long)th;
}

void plat_thread_join(long h) { pthread_join((pthread_t)h, 0); }
void plat_thread_exit(void) { pthread_exit(0); }
void plat_yield(void) { sched_yield(); }
void plat_sleep_ms(long ms) { usleep(ms * 1000); }
long plat_now_us(void) { struct timeval tv; gettimeofday(&tv, 0); return tv.tv_sec * 1000000L + tv.tv_usec; }

// counting semaphore from a mutex + condvar (macOS has no unnamed sem_init)
static struct { pthread_mutex_t m; pthread_cond_t c; int v; } sems[256];
static int nsems;
int
plat_sem_create(void)
{
  int i = __atomic_fetch_add(&nsems, 1, __ATOMIC_SEQ_CST);
  pthread_mutex_init(&sems[i].m, 0);
  pthread_cond_init(&sems[i].c, 0);
  return i;
}
void
plat_sem_wait(int s)
{
  pthread_mutex_lock(&sems[s].m);
  while(sems[s].v == 0) pthread_cond_wait(&sems[s].c, &sems[s].m);
  sems[s].v--;
  pthread_mutex_unlock(&sems[s].m);
}
void
plat_sem_post(int s)
{
  pthread_mutex_lock(&sems[s].m);
  sems[s].v++;
  pthread_cond_signal(&sems[s].c);
  pthread_mutex_unlock(&sems[s].m);
}
