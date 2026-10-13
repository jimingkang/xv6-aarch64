#ifndef _UAPI_SCHED_H
#define _UAPI_SCHED_H

#include "types.h"

#define SCHED_OTHER 0
#define SCHED_FIFO  1
#define SCHED_RR    2

#define SCHED_PRIO_MIN 1
#define SCHED_PRIO_MAX 99
#define NICE_MIN (-20)
#define NICE_MAX 19
#define SCHED_TICK_MS 10

struct sched_info {
  int pid;
  int policy;
  int priority;
  int nice;
  uint cpumask;
  int cpu;
  int state;
  uint64 run_ticks;
};

#endif
