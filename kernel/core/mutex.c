#include <kernel/types.h>
#include "aarch64.h"
#include <kernel/defs.h>
#include <kernel/param.h>
#include <kernel/spinlock.h>
#include <kernel/proc.h>
#include <kernel/mutex.h>

// Waiters live on the blocked process's kernel stack.  The unlocker removes a
// waiter from the queue before granting it, so no queue retains the address
// after mutex_lock() returns.
struct mutex_waiter {
  struct mutex_waiter *next;
  struct proc *task;
  int granted;
};

void
mutex_init(struct mutex *m, char *name)
{
  initlock(&m->wait_lock, "mutex-wait");
  m->owner = 0;
  m->boot_owner = 0;
  m->head = m->tail = 0;
  m->name = name;
}

void
mutex_lock(struct mutex *m)
{
  struct proc *p = myproc();
  struct cpu *boot = p ? 0 : &cpus[cpuid()];
  struct mutex_waiter waiter;

  acquire(&m->wait_lock);
  if((p && m->owner == p) || (boot && m->boot_owner == boot))
    panic("mutex_lock: recursive");
  if(m->owner == 0 && m->boot_owner == 0 && m->head == 0){
    m->owner = p;
    m->boot_owner = boot;
    release(&m->wait_lock);
    return;
  }
  // sleep() requires a current process.  Early boot and idle/IRQ context may
  // use a mutex only when it can be acquired immediately.
  if(p == 0){
    release(&m->wait_lock);
    panic("mutex_lock: contention without process");
  }

  waiter.next = 0;
  waiter.task = p;
  waiter.granted = 0;
  if(m->tail)
    m->tail->next = &waiter;
  else
    m->head = &waiter;
  m->tail = &waiter;
  while(!waiter.granted)
    sleep(&waiter, &m->wait_lock);
  // Ownership was handed directly to us by mutex_unlock().
  release(&m->wait_lock);
}

int
mutex_trylock(struct mutex *m)
{
  struct proc *p = myproc();
  struct cpu *boot = p ? 0 : &cpus[cpuid()];
  int ok = 0;

  acquire(&m->wait_lock);
  if(m->owner == 0 && m->boot_owner == 0 && m->head == 0){
    m->owner = p;
    m->boot_owner = boot;
    ok = 1;
  }
  release(&m->wait_lock);
  return ok;
}

void
mutex_unlock(struct mutex *m)
{
  struct proc *p = myproc();
  struct cpu *boot = p ? 0 : &cpus[cpuid()];
  struct mutex_waiter *w;

  acquire(&m->wait_lock);
  if((p && m->owner != p) || (boot && m->boot_owner != boot))
    panic("mutex_unlock: not owner");
  m->boot_owner = 0;
  w = m->head;
  if(w){
    m->head = w->next;
    if(m->head == 0)
      m->tail = 0;
    m->owner = w->task;
    w->granted = 1;
    wakeup(w);
  } else {
    m->owner = 0;
  }
  release(&m->wait_lock);
}

int
mutex_is_owned(struct mutex *m)
{
  struct proc *p = myproc();
  struct cpu *boot = p ? 0 : &cpus[cpuid()];
  int held;
  acquire(&m->wait_lock);
  held = (p && m->owner == p) || (boot && m->boot_owner == boot);
  release(&m->wait_lock);
  return held;
}
