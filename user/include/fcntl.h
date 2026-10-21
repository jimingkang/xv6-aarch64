#ifndef _FCNTL_H
#define _FCNTL_H

#include <sys/types.h>
#include <uapi/fcntl.h>

#define O_CREAT O_CREATE

int open(const char *, int, ...);
int fcntl(int, int, ...);

#endif
