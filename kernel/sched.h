#ifndef XV6_SCHED_H
#define XV6_SCHED_H

// Scheduling policies.  The values match Linux so user programs read the same.
//
//   SCHED_OTHER  time-sharing.  nice (-20..19) only changes the time slice;
//                every SCHED_OTHER task runs below every real-time task.
//   SCHED_FIFO   fixed-priority real time.  Runs until it blocks, yields or a
//                higher-priority task becomes runnable.  No time slice.
//   SCHED_RR     like SCHED_FIFO, but tasks of equal priority rotate every
//                SCHED_RR_TIMESLICE scheduler ticks.
#define SCHED_OTHER 0
#define SCHED_FIFO  1
#define SCHED_RR    2

#define SCHED_PRIO_MIN 1      // real-time priority, larger runs first
#define SCHED_PRIO_MAX 99
#define NICE_MIN (-20)
#define NICE_MAX 19

#define SCHED_TICK_MS 10      // the per-CPU Generic Timer interrupt period

// Snapshot returned by sched_getinfo().
struct sched_info {
  int pid;
  int policy;
  int priority;      // 1..99 for SCHED_FIFO/SCHED_RR, 0 for SCHED_OTHER
  int nice;
  uint cpumask;      // bit n set: may run on CPU n
  int cpu;           // CPU running it now, else the last one, -1 if never ran
  int state;
  uint64 run_ticks;  // scheduler ticks (SCHED_TICK_MS each) spent running
};

#endif
