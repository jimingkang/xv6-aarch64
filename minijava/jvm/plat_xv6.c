// mjvm platform layer for xv6-aarch64 (java branch syscalls).
#include <uapi/types.h>
#include <uapi/stat.h>
#include <uapi/fcntl.h>
#include "user/user.h"

#define SYNC_SEM 2

void *
plat_chunk(long size)
{
  char *p = sbrk(size);
  if(p == (char *)-1) return 0;
  memset(p, 0, size);
  return p;
}

int
plat_read_file(const char *path, char **data, int *len)
{
  struct stat st;
  int fd = open(path, O_RDONLY);
  if(fd < 0) return -1;
  if(fstat(fd, &st) < 0){ close(fd); return -1; }
  char *buf = sbrk((st.size + 16 + 15) & ~15);
  if(buf == (char *)-1){ close(fd); return -1; }
  int n = 0, r;
  while(n < (int)st.size && (r = read(fd, buf + n, st.size - n)) > 0) n += r;
  close(fd);
  *data = buf;
  *len = n;
  return 0;
}

void plat_write(const char *s, int n) { write(1, s, n); }
void plat_exit(int code) { exit(code); }

long
plat_thread_start(void (*fn)(void *), void *arg, char *stack, long stacksize)
{
  return thread_clone(fn, arg, (void *)(((unsigned long)stack + stacksize) & ~15UL));
}

void plat_thread_join(long h) { tjoin((int)h, 0); }
void plat_thread_exit(void) { texit(0); }
void plat_yield(void) { sched_yield(); }
void plat_sleep_ms(long ms) { if(ms > 0) usleep(ms * 1000); }
long plat_now_us(void) { return (long)clock_us(); }

int plat_sem_create(void) { return sync_create(SYNC_SEM, 0); }
void plat_sem_wait(int s) { sync_wait(s); }
void plat_sem_post(int s) { sync_signal(s, 1); }
