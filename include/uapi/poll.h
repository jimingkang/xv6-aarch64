#ifndef _UAPI_POLL_H
#define _UAPI_POLL_H

#define POLLIN   0x001
#define POLLOUT  0x004
#define POLLERR  0x008
#define POLLNVAL 0x020

struct pollfd {
  int fd;
  short events;
  short revents;
};

#endif
