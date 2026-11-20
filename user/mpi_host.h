// mpi_host.h -- lets user/mpi.c build natively (macOS, Linux) with -DMPI_HOST.
//
// The ranks of the host node are forked and share memory exactly as on xv6.
// What xv6 provides as system calls is mapped to POSIX here: the per-rank
// doorbell semaphore becomes a short nap (a producer's "signal" is then a
// no-op), CPU pinning is skipped, and sockets are BSD sockets.
#ifndef MPI_HOST_H
#define MPI_HOST_H

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "include/mpi.h"

typedef unsigned int uint;
typedef unsigned long uint64;

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#define NCPU 1024
#define SYNC_SEM 0

#define fprintf(fd, ...) dprintf((fd), __VA_ARGS__)
#define kill(pid) kill((pid), SIGKILL)

static inline int sync_create(int kind, int value) { (void)kind; (void)value; return 0; }
static inline int sync_destroy(int h) { (void)h; return 0; }
static inline int sync_signal(int h, int n) { (void)h; (void)n; return 0; }
static inline int sync_wait(int h) { (void)h; usleep(50); return 0; }
static inline int sched_setaffinity_xv6(int pid, unsigned mask) { (void)pid; (void)mask; return 0; }
#define sched_setaffinity(pid, mask) sched_setaffinity_xv6((pid), (mask))

static inline int getcpu(void) { return -1; }

static inline unsigned long
clock_us(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (unsigned long)t.tv_sec * 1000000UL + (unsigned long)t.tv_nsec / 1000UL;
}

static inline void
host_name(char *s, int n)
{
  if(gethostname(s, n) < 0)
    snprintf(s, n, "host");
  s[n - 1] = 0;
}

static inline int
host_connect(const char *host, int port)
{
  struct sockaddr_in a;
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if(fd < 0)
    return -1;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = inet_addr(host);
  if(connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0){
    close(fd);
    return -1;
  }
  return fd;
}

static inline int
host_listen(int port, int backlog)
{
  struct sockaddr_in a;
  int one = 1, fd = socket(AF_INET, SOCK_STREAM, 0);
  if(fd < 0)
    return -1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  if(bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, backlog) < 0){
    close(fd);
    return -1;
  }
  return fd;
}

static inline int
host_accept(int fd)
{
  return accept(fd, 0, 0);
}

static inline void
host_tune_socket(int fd)
{
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
}

// Wait up to `ms` for any connected peer to have data.
static inline void
host_poll_peers(const int *fds, int n, int ms)
{
  struct pollfd p[MPI_MAX_RANKS];
  int k = 0;
  for(int i = 0; i < n; i++)
    if(fds[i] >= 0){
      p[k].fd = fds[i];
      p[k].events = POLLIN;
      p[k].revents = 0;
      k++;
    }
  if(k)
    poll(p, k, ms);
}

#endif
