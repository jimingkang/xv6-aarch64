#ifndef _SYS_RESOURCE_H
#define _SYS_RESOURCE_H

// getrusage() for xv6.  The kernel keeps no resource accounting, so only
// ru_maxrss is filled in: the current program break (heap + data) in KB,
// Linux's unit.  The time fields are zero.

#include <sys/types.h>

#define RUSAGE_SELF 0

struct timeval_ru {
  long tv_sec;
  long tv_usec;
};

struct rusage {
  struct timeval_ru ru_utime;
  struct timeval_ru ru_stime;
  long ru_maxrss;
};

#ifdef __cplusplus
extern "C" char *sbrk(int);
#else
char *sbrk(int);
#endif

static inline int
getrusage(int who, struct rusage *usage)
{
  if(who != RUSAGE_SELF || usage == 0)
    return -1;
  usage->ru_utime.tv_sec = usage->ru_utime.tv_usec = 0;
  usage->ru_stime.tv_sec = usage->ru_stime.tv_usec = 0;
  usage->ru_maxrss = (long)(unsigned long)sbrk(0) / 1024;
  return 0;
}

#endif
