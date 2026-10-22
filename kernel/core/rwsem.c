#include <kernel/types.h>
#include "aarch64.h"
#include <kernel/defs.h>
#include <kernel/param.h>
#include <kernel/spinlock.h>
#include <kernel/proc.h>
#include <kernel/rwsem.h>

struct rwsem_waiter {
  struct rwsem_waiter *next;
  struct proc *task;
  int writer;
  int granted;
};

static void
rwsem_enqueue(struct rw_semaphore *sem, struct rwsem_waiter *w)
{
  w->next = 0;
  if(sem->tail)
    sem->tail->next = w;
  else
    sem->head = w;
  sem->tail = w;
}

// Called with wait_lock held and no active writer.  Preserve FIFO ordering:
// grant one writer, or grant the complete reader run at the queue head.
static void
rwsem_grant(struct rw_semaphore *sem)
{
  struct rwsem_waiter *w;

  if(sem->writer || sem->boot_writer || sem->head == 0)
    return;
  if(sem->head->writer){
    if(sem->readers != 0)
      return;
    w = sem->head;
    sem->head = w->next;
    if(sem->head == 0)
      sem->tail = 0;
    sem->writer = w->task;
    w->granted = 1;
    wakeup(w);
    return;
  }
  while((w = sem->head) != 0 && !w->writer){
    sem->head = w->next;
    if(sem->head == 0)
      sem->tail = 0;
    sem->readers++;
    w->granted = 1;
    wakeup(w);
  }
}

void
init_rwsem(struct rw_semaphore *sem, char *name)
{
  initlock(&sem->wait_lock, "rwsem-wait");
  sem->readers = 0;
  sem->writer = 0;
  sem->boot_writer = 0;
  sem->head = sem->tail = 0;
  sem->name = name;
}

void
down_read(struct rw_semaphore *sem)
{
  struct proc *p = myproc();
  struct rwsem_waiter waiter;

  acquire(&sem->wait_lock);
  if(sem->writer == 0 && sem->boot_writer == 0 && sem->head == 0){
    sem->readers++;
    release(&sem->wait_lock);
    return;
  }
  if(p == 0){
    release(&sem->wait_lock);
    panic("down_read: contention without process");
  }
  waiter.task = p;
  waiter.writer = 0;
  waiter.granted = 0;
  rwsem_enqueue(sem, &waiter);
  while(!waiter.granted)
    sleep(&waiter, &sem->wait_lock);
  release(&sem->wait_lock);
}

int
down_read_trylock(struct rw_semaphore *sem)
{
  int ok = 0;
  acquire(&sem->wait_lock);
  if(sem->writer == 0 && sem->boot_writer == 0 && sem->head == 0){
    sem->readers++;
    ok = 1;
  }
  release(&sem->wait_lock);
  return ok;
}

void
up_read(struct rw_semaphore *sem)
{
  acquire(&sem->wait_lock);
  if(sem->readers <= 0)
    panic("up_read");
  sem->readers--;
  if(sem->readers == 0)
    rwsem_grant(sem);
  release(&sem->wait_lock);
}

void
down_write(struct rw_semaphore *sem)
{
  struct rwsem_waiter waiter;
  struct proc *p = myproc();
  struct cpu *boot = p ? 0 : &cpus[cpuid()];

  acquire(&sem->wait_lock);
  if((p && sem->writer == p) || (boot && sem->boot_writer == boot))
    panic("down_write: recursive");
  if(sem->writer == 0 && sem->boot_writer == 0 && sem->readers == 0 &&
     sem->head == 0){
    sem->writer = p;
    sem->boot_writer = boot;
    release(&sem->wait_lock);
    return;
  }
  if(p == 0){
    release(&sem->wait_lock);
    panic("down_write: contention without process");
  }
  waiter.task = p;
  waiter.writer = 1;
  waiter.granted = 0;
  rwsem_enqueue(sem, &waiter);
  while(!waiter.granted)
    sleep(&waiter, &sem->wait_lock);
  release(&sem->wait_lock);
}

int
down_write_trylock(struct rw_semaphore *sem)
{
  struct proc *p = myproc();
  struct cpu *boot = p ? 0 : &cpus[cpuid()];
  int ok = 0;
  acquire(&sem->wait_lock);
  if(sem->writer == 0 && sem->boot_writer == 0 && sem->readers == 0 &&
     sem->head == 0){
    sem->writer = p;
    sem->boot_writer = boot;
    ok = 1;
  }
  release(&sem->wait_lock);
  return ok;
}

void
up_write(struct rw_semaphore *sem)
{
  struct proc *p = myproc();
  struct cpu *boot = p ? 0 : &cpus[cpuid()];

  acquire(&sem->wait_lock);
  if((p && sem->writer != p) || (boot && sem->boot_writer != boot))
    panic("up_write: not owner");
  sem->writer = 0;
  sem->boot_writer = 0;
  rwsem_grant(sem);
  release(&sem->wait_lock);
}

int
rwsem_write_held(struct rw_semaphore *sem)
{
  struct proc *p = myproc();
  struct cpu *boot = p ? 0 : &cpus[cpuid()];
  int held;
  acquire(&sem->wait_lock);
  held = (p && sem->writer == p) ||
         (boot && sem->boot_writer == boot);
  release(&sem->wait_lock);
  return held;
}
