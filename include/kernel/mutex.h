#ifndef XV6_MUTEX_H
#define XV6_MUTEX_H

#include <kernel/spinlock.h>

struct proc;
struct cpu;

// A sleepable, FIFO mutual-exclusion lock.  wait_lock is held only while
// changing the owner and waiter list; the protected critical section runs
// with interrupts enabled and may sleep.
struct mutex_waiter;
struct mutex {
  struct spinlock wait_lock;
  struct proc *owner;
  // Before scheduler() installs a current process, early initcalls still
  // need uncontended locking.  Such an owner is identified by CPU instead;
  // it is never allowed to block on this sleepable mutex.
  struct cpu *boot_owner;
  struct mutex_waiter *head;
  struct mutex_waiter *tail;
  char *name;
};

#endif
