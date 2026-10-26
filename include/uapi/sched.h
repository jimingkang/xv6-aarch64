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

// clone(2) flags, with Linux's values.  The low byte (CSIGNAL) is the signal
// sent to the parent at exit; xv6 accepts 0 or SIGCHLD there and always
// notifies wait().
#define CSIGNAL              0x000000ff
#define CLONE_VM             0x00000100  // share the address space
#define CLONE_FS             0x00000200  // share cwd (fs_struct)
#define CLONE_FILES          0x00000400  // share the descriptor table
#define CLONE_SIGHAND        0x00000800  // share signal handlers
#define CLONE_VFORK          0x00004000  // parent sleeps until child exec/exit
#define CLONE_THREAD         0x00010000  // same thread group as the caller
#define CLONE_PARENT_SETTID  0x00100000  // store child TID at ptid (parent)
#define CLONE_CHILD_SETTID   0x01000000  // store child TID at ctid (child)
#ifndef SIGCHLD
#define SIGCHLD 17
#endif

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
