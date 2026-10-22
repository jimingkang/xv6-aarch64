#ifndef XV6_RWSEM_H
#define XV6_RWSEM_H

#include <kernel/spinlock.h>

struct proc;
struct cpu;

// Sleepable reader/writer semaphore with FIFO ordering.  A run of readers at
// the head of the queue is granted together; a queued writer prevents later
// readers from bypassing it.
struct rwsem_waiter;
struct rw_semaphore {
  struct spinlock wait_lock;
  int readers;
  struct proc *writer;
  struct cpu *boot_writer;
  struct rwsem_waiter *head;
  struct rwsem_waiter *tail;
  char *name;
};

#endif
