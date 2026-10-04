#ifndef _SYS_EPOLL_H
#define _SYS_EPOLL_H

#include "kernel/epoll.h"

int epoll_create1(int);
int epoll_ctl(int, int, int, struct epoll_event *);
int epoll_wait(int, struct epoll_event *, int, int);

#endif
