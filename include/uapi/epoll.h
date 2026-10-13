#ifndef _UAPI_EPOLL_H
#define _UAPI_EPOLL_H

#include "types.h"

#define EPOLLIN   0x001
#define EPOLLOUT  0x004
#define EPOLLERR  0x008
#define EPOLLET   (1U << 31)

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

/* Current ABI limit returned/enforced by the compact xv6 epoll object. */
#define EPOLL_MAX_WATCH 16

typedef union epoll_data {
  void *ptr;
  int fd;
  uint32 u32;
  uint64 u64;
} epoll_data_t;

struct epoll_event {
  uint32 events;
  uint32 pad;
  epoll_data_t data;
};

#endif
