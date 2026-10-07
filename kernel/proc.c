#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "aarch64.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "proc.h"
#include "sched.h"
#include "camera.h"
#include "defs.h"

struct cpu cpus[NCPU];
struct proc *proc_head;
static struct spinlock proc_table_lock;
static struct spinlock kernel_stack_lock;
static uint64 next_kstack_slot;

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);
static void threadexit(int status) __attribute__((noreturn));
static void reparent(struct proc *p);

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

extern pagetable_t kernel_pagetable;

static struct vmspace*
vmspace_alloc(void)
{
  struct vmspace *vm = kalloc();
  if(vm == 0)
    return 0;
  memset(vm, 0, sizeof(*vm));
  initlock(&vm->lock, "vmspace");
  vm->pagetable = uvmcreate();
  if(vm->pagetable == 0){
    kfree(vm);
    return 0;
  }
  vm->refcount = 1;
  return vm;
}

static void
vmspace_put(struct vmspace *vm)
{
  int last;
  pagetable_t pagetable = 0;
  uint64 sz = 0;

  if(vm == 0)
    return;
  acquire(&vm->lock);
  if(vm->refcount < 1)
    panic("vmspace_put");
  last = --vm->refcount == 0;
  if(last){
    pagetable = vm->pagetable;
    sz = vm->sz;
    vm->pagetable = 0;
    vm->sz = 0;
  }
  release(&vm->lock);
  if(last){
    uvmfree(pagetable, sz);
    kfree(vm);
  }
}

// ---------------------------------------------------------------------------
// Priority scheduler with CPU affinity.
//
// Every runnable process has an effective priority:
//
//     PRIO_RT_BASE + rt_priority   SCHED_FIFO / SCHED_RR   (101..199)
//     PRIO_OTHER                   SCHED_OTHER             (0)
//     PRIO_THROTTLED               real-time task on a CPU whose RT budget
//                                  for this period is used up            (-1)
//
// Each CPU runs the highest-priority RUNNABLE process whose cpumask allows
// that CPU; ties go to the smallest rq_seq, i.e. the one queued first.  The
// The run "queue" is still the growable process list plus a sequence number,
// rather than a separate container.
//
// Preemption: a process that becomes runnable compares its priority with the
// CPUs it may use.  If one of them runs something lower, that CPU gets
// need_resched and, when it is another core, a mailbox IPI, so it switches
// within microseconds instead of at its next 10 ms timer tick.
//
// RT throttling (as Linux sched_rt_runtime_us/sched_rt_period_us): per CPU, real-time
// tasks may use at most RT_RUNTIME of every RT_PERIOD ticks.  After that the
// CPU prefers SCHED_OTHER tasks until the period ends, so a runaway SCHED_FIFO
// loop cannot lock out the shell.
// ---------------------------------------------------------------------------
#define PRIO_IDLE      (-1000)
#define PRIO_THROTTLED (-1)
#define PRIO_OTHER     0
#define PRIO_RT_BASE   100

#define RR_TIMESLICE   10     // 100 ms
#define RT_PERIOD      100    // 1 s
#define RT_RUNTIME     95     // 950 ms
#define FIFO_SLICE     (1 << 30)
#define ALL_CPUS       ((1U << NCPU) - 1)

static uint64 rq_counter;
static struct spinlock rq_counter_lock;   // GCC atomics may need libatomic

static int
is_rt_policy(int policy)
{
  return policy == SCHED_FIFO || policy == SCHED_RR;
}

static uint64
next_rq_seq(void)
{
  uint64 seq;
  acquire(&rq_counter_lock);
  seq = ++rq_counter;
  release(&rq_counter_lock);
  return seq;
}

static int
timeslice(struct proc *p)
{
  if(p->policy == SCHED_FIFO)
    return FIFO_SLICE;
  if(p->policy == SCHED_RR)
    return RR_TIMESLICE;
  // SCHED_OTHER: nice 0 keeps xv6's 100 ms; -20 -> 200 ms, 19 -> 10 ms.
  int s = (20 - p->nice) / 2;
  return s < 1 ? 1 : s;
}

// Priority ignoring throttling: used when deciding whom to preempt.
static int
base_prio(struct proc *p)
{
  if(is_rt_policy(p->policy))
    return PRIO_RT_BASE + p->rt_priority;
  return PRIO_OTHER;
}

static int
effective_prio(struct proc *p, struct cpu *c)
{
  if(is_rt_policy(p->policy) && c->rt_throttled)
    return PRIO_THROTTLED;
  return base_prio(p);
}

static void
kick_cpu(int cpu)
{
  cpus[cpu].need_resched = 1;
  __sync_synchronize();
  if(cpu != cpuid())
    send_resched_ipi(cpu);
}

// p just became RUNNABLE (caller holds p->lock).  Preempt the allowed CPU
// running the lowest priority if p beats it.
static void
resched_hint(struct proc *p)
{
  int prio = base_prio(p);
  int target = -1, lowest = 0;

  for(int i = 0; i < NCPU; i++){
    if(!(p->cpumask & (1U << i)) || !cpus[i].online)
      continue;
    int cp = cpus[i].cur_prio;
    if(cp == PRIO_IDLE)
      return;               // an allowed CPU is idle and will pick p up
    if(target < 0 || cp < lowest){
      target = i;
      lowest = cp;
    }
  }
  if(target >= 0 && prio > lowest)
    kick_cpu(target);
}

// Put p on the run queue (caller holds p->lock).  at_head keeps its old
// position among equal priorities: used when it was preempted rather than
// having used up its slice, as Linux does for SCHED_FIFO/SCHED_RR.
static void
make_runnable(struct proc *p, int at_head)
{
  p->state = RUNNABLE;
  if(!at_head || p->rq_seq == 0)
    p->rq_seq = next_rq_seq();
  resched_hint(p);
}

static void
sched_defaults(struct proc *p)
{
  p->policy = SCHED_OTHER;
  p->rt_priority = 0;
  p->nice = 0;
  p->cpumask = ALL_CPUS;
  p->slice = timeslice(p);
  p->rq_seq = 0;
  p->last_cpu = -1;
  p->run_ticks = 0;
}

// Called from the timer interrupt on every CPU, every SCHED_TICK_MS.
void
sched_tick(void)
{
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  int rt = 0;

  if(p){
    acquire(&p->lock);
    rt = is_rt_policy(p->policy);
    p->run_ticks++;
    if(p->policy != SCHED_FIFO && --p->slice <= 0)
      c->need_resched = 1;
    release(&p->lock);
  }

  c->rt_window++;
  if(rt)
    c->rt_used++;
  if(c->rt_window >= RT_PERIOD){
    c->rt_window = 0;
    c->rt_used = 0;
    if(c->rt_throttled){
      c->rt_throttled = 0;      // RT tasks may run again: re-evaluate
      c->need_resched = 1;
    }
  } else if(!c->rt_throttled && c->rt_used >= RT_RUNTIME){
    c->rt_throttled = 1;
    c->rt_throttle_events++;
    if(rt)
      c->need_resched = 1;
  }
}

// Should the current CPU reschedule?  Safe with interrupts on.
int
resched_pending(void)
{
  push_off();
  int r = mycpu()->need_resched;
  pop_off();
  return r;
}

// initialize the proc table at boot time.
void
procinit(void)
{
  initlock(&proc_table_lock, "proc_table");
  initlock(&kernel_stack_lock, "kernel_stack");
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  initlock(&rq_counter_lock, "rq_counter");
  proc_head = 0;
  next_kstack_slot = 0;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void) {
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void) {
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid() {
  int pid;
  
  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Allocate and publish one permanent process-table node.  The descriptor is
// retained when UNUSED so lockless readers of the grow-only list can never
// hold a dangling pointer; its physical kernel-stack page is reclaimed.
// Caller holds proc_table_lock.
static int
alloc_kstack(struct proc *p)
{
  void *stack = kalloc();
  if(stack == 0)
    return -1;

  acquire(&kernel_stack_lock);
  if(mappages(kernel_pagetable, p->kstack, PGSIZE, V2P(stack),
              PTE_NORMAL | PTE_XN) < 0){
    release(&kernel_stack_lock);
    kfree(stack);
    return -1;
  }
  flush_tlb();
  release(&kernel_stack_lock);
  p->kstack_pa = V2P(stack);
  return 0;
}

static void
free_kstack(struct proc *p)
{
  pte_t *pte;
  uint64 pa;

  if(p->kstack_pa == 0)
    return;
  acquire(&kernel_stack_lock);
  pte = walk(kernel_pagetable, p->kstack, 0);
  if(pte == 0 || (*pte & PTE_V) == 0)
    panic("free_kstack");
  pa = PTE2PA(*pte);
  *pte = 0;
  // The exiting task has already switched to its reaper's kernel stack.
  // Invalidate every CPU before the physical page can be reused.
  flush_tlb();
  release(&kernel_stack_lock);
  p->kstack_pa = 0;
  kfree(P2V(pa));
}

static struct proc*
newprocslot(void)
{
  struct proc *p = kalloc();
  uint64 slot, va;

  if(p == 0)
    return 0;
  if(sizeof(*p) > PGSIZE)
    panic("struct proc too large");
  memset(p, 0, PGSIZE);
  initlock(&p->lock, "proc");

  slot = next_kstack_slot++;
  va = KSTACK(slot);
  // Keep dynamically growing stacks in the high sparse KSTACK region and
  // never let an arithmetic wrap collide with the kernel direct map.
  if(va < KERNBASE + (1ULL << 37)){
    kfree(p);
    return 0;
  }
  p->kstack = va;
  p->kstack_slot = slot;
  if(alloc_kstack(p) < 0){
    kfree(p);
    return 0;
  }

  p->next = proc_head;
  __sync_synchronize();
  proc_head = p;
  return p;
}

// Find an UNUSED dynamic slot or grow the process table.  Returns with
// p->lock held.  The only fixed limit is the memory/virtual mapping capacity.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&proc_table_lock);
  for(p = proc_head; p; p = p->next) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      if(p->kstack_pa == 0 && alloc_kstack(p) < 0){
        release(&p->lock);
        release(&proc_table_lock);
        return 0;
      }
      goto found;
    } else {
      release(&p->lock);
    }
  }
  p = newprocslot();
  if(p == 0){
    release(&proc_table_lock);
    return 0;
  }
  acquire(&p->lock);

found:
  release(&proc_table_lock);
  p->pid = allocpid();
  p->tgid = p->pid;
  p->is_thread = 0;
  p->sid = p->pid;
  p->pgid = p->pid;
  p->ctty = -1;
  p->signals_pending = 0;
  sched_defaults(p);
  p->state = USED;

  sp = (char*)p->kstack + PGSIZE;

  // Allocate a trapframe page.
  sp -= sizeof(*p->trapframe);
  p->trapframe = (struct trapframe*)sp;

  // An empty user address space. clone() replaces this reference with its
  // caller's vmspace; fork() keeps it and copies pages into it.
  p->vm = vmspace_alloc();
  if(p->vm == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.x30 = (uint64)forkret;
  p->context.sp = (uint64)sp;

  return p;
}

static void
kthread_entry(void)
{
  struct proc *p = myproc();
  void (*fn)(void*) = p->kthread_fn;
  void *arg = p->kthread_arg;

  // scheduler() enters a fresh context while holding p->lock, just like
  // forkret().  A kernel thread drops it before calling its body.
  release(&p->lock);
  fn(arg);
  panic("kthread returned");
}

int
kthread_create(void (*fn)(void*), void *arg, char *name)
{
  struct proc *p;

  if(fn == 0 || (p = allocproc()) == 0)
    return -1;
  p->kthread_fn = fn;
  p->kthread_arg = arg;
  p->context.x30 = (uint64)kthread_entry;
  safestrcpy(p->name, name ? name : "kthread", sizeof(p->name));
  make_runnable(p, 0);
  int pid = p->pid;
  release(&p->lock);
  return pid;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  p->trapframe = 0;
  vmspace_put(p->vm);
  p->vm = 0;
  p->pid = 0;
  p->tgid = 0;
  p->is_thread = 0;
  p->sid = 0;
  p->pgid = 0;
  p->ctty = -1;
  p->signals_pending = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->kthread_fn = 0;
  p->kthread_arg = 0;
  free_kstack(p);
  p->state = UNUSED;
}

// a user program that calls exec("/init")
// od -t xC initcode
uchar initcode[] = {
  0xc0, 0x01, 0x00, 0x58, 0xe1, 0x01, 0x00, 0x58, 0xe7,
  0x00, 0x80, 0xd2, 0x01, 0x00, 0x00, 0xd4, 0x47, 0x00,
  0x80, 0xd2, 0x01, 0x00, 0x00, 0xd4, 0xfe, 0xff, 0xff,
  0x17, 0x2f, 0x69, 0x6e, 0x69, 0x74, 0x00, 0x00, 0x00,
  0x1f, 0x20, 0x03, 0xd5, 0x1c, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x1c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x28, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;
  
  // allocate one user page and copy init's instructions
  // and data into it.
  uvminit(p->vm->pagetable, initcode, sizeof(initcode));
  p->vm->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->elr = 0;      // user program counter
  p->trapframe->spsr = 0;     // switch to EL0
  p->trapframe->sp = PGSIZE;  // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = 0;              // set by prepare_namespace() in forkret()
  safestrcpy(p->cwdpath, "/", sizeof(p->cwdpath));

  make_runnable(p, 0);

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();
  struct vmspace *vm = p->vm;

  acquire(&vm->lock);
  sz = vm->sz;
  if(n > 0){
    if((sz = uvmalloc(vm->pagetable, sz, sz + n)) == 0) {
      release(&vm->lock);
      return -1;
    }
  } else if(n < 0){
    if((uint64)(-(long)n) > sz){
      release(&vm->lock);
      return -1;
    }
    uint64 newsz = sz + n;
    uint64 first = PGROUNDUP(newsz);
    uint64 end = PGROUNDUP(sz);

    // A shared page table can be active on another Cortex-A53.  Clear PTEs,
    // broadcast the invalidation, and only then recycle their physical pages.
    while(first < end){
      uint64 pa[64];
      int count = 0;
      while(first < end && count < (int)NELEM(pa)){
        pte_t *pte = walk(vm->pagetable, first, 0);
        if(pte == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_AF) == 0)
          panic("growproc dealloc");
        pa[count++] = PTE2PA(*pte);
        *pte = 0;
        first += PGSIZE;
      }
      // VMALLE1IS reaches every PE in the inner-shareable domain.
      flush_tlb();
      for(int i = 0; i < count; i++)
        kfree((void *)P2V(pa[i]));
    }
    sz = newsz;
  }
  vm->sz = sz;

  // Also remove negative translations cached before a heap growth.
  flush_tlb();
  release(&vm->lock);
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy user memory from parent to child.
  acquire(&p->vm->lock);
  if(uvmcopy(p->vm->pagetable, np->vm->pagetable, p->vm->sz) < 0){
    release(&p->vm->lock);
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  if(p->vm->camera_mapped){
    if(mappages(np->vm->pagetable, CAM_MMAP_BASE, CAM_MMAP_BYTES, CAMDMA_PA,
                PTE_NORMAL_NC | PTE_URO | PTE_XN) < 0){
      release(&p->vm->lock);
      freeproc(np);
      release(&np->lock);
      return -1;
    }
    np->vm->camera_mapped = 1;
  }
  np->vm->sz = p->vm->sz;
  release(&p->vm->lock);

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->x0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);
  safestrcpy(np->cwdpath, p->cwdpath, sizeof(np->cwdpath));
  np->sid = p->sid;
  np->pgid = p->pgid;
  np->ctty = p->ctty;
  np->tgid = np->pid;

  safestrcpy(np->name, p->name, sizeof(p->name));

  // Like Linux, a child inherits policy, priority, nice and affinity.
  acquire(&p->lock);
  np->policy = p->policy;
  np->rt_priority = p->rt_priority;
  np->nice = p->nice;
  np->cpumask = p->cpumask;
  release(&p->lock);
  np->slice = timeslice(np);

  // uvmcopy allocated a separate set of pages and PTEs for the child.
  // uvmdump(np->pagetable, np->pid, np->name, "fork-copy");

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  make_runnable(np, 0);
  release(&np->lock);

  return pid;
}

// Create a new schedulable user thread in the caller's address space.
// entry receives arg in x0 and must terminate with texit(); the user library
// supplies a wrapper that guarantees this when the thread function returns.
int
threadclone(uint64 entry, uint64 arg, uint64 stack_top)
{
  int i, tid;
  struct proc *np;
  struct proc *p = myproc();
  struct vmspace *vm = p->vm;

  if(entry == 0 || stack_top < 16 || (stack_top & 15) != 0)
    return -1;
  if((np = allocproc()) == 0)
    return -1;

  acquire(&vm->lock);
  if(vm->execing || entry >= vm->sz || stack_top > vm->sz ||
     walkaddr(vm->pagetable, entry) == 0 ||
     walkaddr(vm->pagetable, stack_top - 1) == 0){
    release(&vm->lock);
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  vm->refcount++;
  release(&vm->lock);

  // Drop allocproc's private empty vmspace and install the shared one.
  vmspace_put(np->vm);
  np->vm = vm;
  np->is_thread = 1;
  np->tgid = p->tgid;

  *(np->trapframe) = *(p->trapframe);
  np->trapframe->elr = entry;
  np->trapframe->sp = stack_top;
  np->trapframe->x0 = arg;
  np->trapframe->x30 = 0;

  // This first stage duplicates descriptor references, like fork().  The
  // underlying open file descriptions remain shared, but descriptor-table
  // edits themselves are per thread until struct files is introduced.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);
  safestrcpy(np->cwdpath, p->cwdpath, sizeof(np->cwdpath));
  np->sid = p->sid;
  np->pgid = p->pgid;
  np->ctty = p->ctty;
  safestrcpy(np->name, p->name, sizeof(np->name));

  acquire(&p->lock);
  np->policy = p->policy;
  np->rt_priority = p->rt_priority;
  np->nice = p->nice;
  np->cpumask = p->cpumask;
  release(&p->lock);
  np->slice = timeslice(np);

  tid = np->pid;
  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);
  make_runnable(np, 0);
  release(&np->lock);
  return tid;
}

static void
threadexit(int status)
{
  struct proc *p = myproc();

  net_udp_closeproc(p->pid);
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      fileclose(p->ofile[fd]);
      p->ofile[fd] = 0;
    }
  }
  if(p->cwd){
    begin_op();
    iput(p->cwd);
    end_op();
    p->cwd = 0;
  }

  acquire(&wait_lock);
  // Children forked by this thread must not retain a parent pointer to a proc
  // slot that thread_join() will recycle.
  reparent(p);
  wakeup(initproc);
  // Wake before publishing ZOMBIE, as process exit does.  The joiner cannot
  // rescan until wait_lock is released, and wakeup() cannot deadlock trying
  // to reacquire this thread's p->lock.
  wakeup(p->vm);
  acquire(&p->lock);
  p->xstate = status;
  p->state = ZOMBIE;
  release(&wait_lock);
  sched();
  panic("thread zombie exit");
}

// Join a user thread in this thread group. tid == 0 selects any zombie.
int
threadjoin(int tid, uint64 status_addr)
{
  struct proc *p = myproc();
  struct proc *t;
  int found, joined;

  acquire(&wait_lock);
  for(;;){
    found = 0;
    for(t = proc_head; t; t = t->next){
      if(t == p)
        continue;
      acquire(&t->lock);
      if(t->is_thread && t->tgid == p->tgid &&
         (tid == 0 || t->pid == tid)){
        found = 1;
        if(t->state == ZOMBIE){
          joined = t->pid;
          if(status_addr != 0 &&
             copyout(p->vm->pagetable, status_addr,
                     (char *)&t->xstate, sizeof(t->xstate)) < 0){
            release(&t->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(t);
          release(&t->lock);
          release(&wait_lock);
          return joined;
        }
      }
      release(&t->lock);
    }
    if(!found || p->killed){
      release(&wait_lock);
      return -1;
    }
    sleep(p->vm, &wait_lock);
  }
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc_head; pp; pp = pp->next){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
exit(int status)
{
  struct proc *p = myproc();

  if(p->is_thread)
    threadexit(status);

  if(p == initproc)
    panic("init exiting");

  // exit() is process-wide. Ask sibling threads to leave, then reap their
  // proc slots before publishing the leader as a zombie. Killed sleepers are
  // made runnable so they can observe p->killed at the kernel/user boundary.
  acquire(&wait_lock);
  for(struct proc *t = proc_head; t; t = t->next){
    if(t == p)
      continue;
    acquire(&t->lock);
    if(t->is_thread && t->tgid == p->tgid && t->state != UNUSED &&
       t->state != ZOMBIE){
      t->killed = 1;
      if(t->state == SLEEPING)
        make_runnable(t, 0);
    }
    release(&t->lock);
  }
  for(;;){
    int live = 0;
    for(struct proc *t = proc_head; t; t = t->next){
      if(t == p)
        continue;
      acquire(&t->lock);
      if(t->is_thread && t->tgid == p->tgid && t->state != UNUSED){
        if(t->state == ZOMBIE)
          freeproc(t);
        else
          live = 1;
      }
      release(&t->lock);
    }
    if(!live)
      break;
    sleep(p->vm, &wait_lock);
  }
  release(&wait_lock);

  net_udp_closeproc(p->pid);

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);
  
  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(uint64 addr)
{
  struct proc *np;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(np = proc_head; np; np = np->next){
      if(np->parent == p && !np->is_thread){
        // make sure the child isn't still in exit() or swtch().
        acquire(&np->lock);

        havekids = 1;
        if(np->state == ZOMBIE){
          // Found one.
          pid = np->pid;
          if(addr != 0 && copyout(p->vm->pagetable, addr, (char *)&np->xstate,
                                  sizeof(np->xstate)) < 0) {
            release(&np->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(np);
          release(&np->lock);
          release(&wait_lock);
          return pid;
        }
        release(&np->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || p->killed){
      release(&wait_lock);
      return -1;
    }
    
    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
// Highest effective priority RUNNABLE process allowed on this CPU; ties go
// to the earliest rq_seq.  Returns it unlocked: the caller must re-check.
static struct proc*
pick_next(struct cpu *c, int id)
{
  struct proc *best = 0;
  int best_prio = 0;
  uint64 best_seq = 0;

  for(struct proc *p = proc_head; p; p = p->next){
    acquire(&p->lock);
    if(p->state == RUNNABLE && (p->cpumask & (1U << id))){
      int prio = effective_prio(p, c);
      if(best == 0 || prio > best_prio ||
         (prio == best_prio && p->rq_seq < best_seq)){
        best = p;
        best_prio = prio;
        best_seq = p->rq_seq;
      }
    }
    release(&p->lock);
  }
  return best;
}

void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  int id = cpuid();

  c->proc = 0;
  c->cur_prio = PRIO_IDLE;
  c->online = 1;
  for(;;){
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();

    if((p = pick_next(c, id)) == 0)
      continue;
    acquire(&p->lock);
    // Another CPU may have taken it, or its affinity may have changed.
    if(p->state == RUNNABLE && (p->cpumask & (1U << id))){
      // Switch to chosen process.  It is the process's job
      // to release its lock and then reacquire it
      // before jumping back to us.
      if(p->slice <= 0)
        p->slice = timeslice(p);
      p->state = RUNNING;
      p->last_cpu = id;
      c->proc = p;
      c->cur_prio = effective_prio(p, c);
      c->need_resched = 0;
      switchuvm(p);
      swtch(&c->context, &p->context);

      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
      c->cur_prio = PRIO_IDLE;
    }
    release(&p->lock);
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
//
// A process preempted with time left in its slice keeps its place among
// equal priorities; one whose slice ran out goes behind them (round robin).
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  if(p->slice <= 0)
    p->slice = timeslice(p);
  else if(p->policy == SCHED_FIFO || p->policy == SCHED_RR){
    make_runnable(p, 1);
    sched();
    release(&p->lock);
    return;
  }
  make_runnable(p, 0);
  sched();
  release(&p->lock);
}

// sched_yield(): go behind every runnable process of the same priority.
void
sched_yield_now(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->slice = timeslice(p);
  make_runnable(p, 0);
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  static int first = 1;
  struct proc *p = myproc();
  struct trapframe *tf = p->trapframe;

  // Still holding p->lock from scheduler.
  release(&p->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    first = 0;
    prepare_namespace();   // rootfs, devtmpfs, mount root=, MS_MOVE, chroot
    blkdev_selftest();
  }

  usertrapret(tf);
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock);  //DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void
wakeup(void *chan)
{
  struct proc *p;

  for(p = proc_head; p; p = p->next) {
    if(p != myproc()){
      acquire(&p->lock);
      if(p->state == SLEEPING && p->chan == chan) {
        make_runnable(p, 0);
      }
      release(&p->lock);
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kill(int pid)
{
  struct proc *p;
  int found = 0;

  for(p = proc_head; p; p = p->next){
    acquire(&p->lock);
    // kill() addresses the process ID/TGID.  All execution contexts sharing
    // that process are marked, matching the process-wide signal semantics.
    if(p->state != UNUSED && p->tgid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        // Wake process from sleep().
        make_runnable(p, 0);
      }
      found = 1;
    }
    release(&p->lock);
  }
  return found ? 0 : -1;
}

void
signal_pgrp(int pgid, int sig)
{
  struct proc *p;

  if(pgid <= 0 || sig <= 0 || sig > 32)
    return;
  for(p = proc_head; p; p = p->next){
    acquire(&p->lock);
    if(p->state != UNUSED && p->pgid == pgid && p->ctty == TTYS0){
      p->signals_pending |= 1U << (sig - 1);
      p->killed = 1;
      if(p->state == SLEEPING)
        make_runnable(p, 0);
    }
    release(&p->lock);
  }
}

// ---------------------------------------------------------------------------
// sched_* system call back ends.  pid 0 means the calling process.
// ---------------------------------------------------------------------------

// Returns with p->lock held, or 0.
static struct proc*
lock_proc_by_pid(int pid)
{
  struct proc *p;

  if(pid == 0){
    p = myproc();
    acquire(&p->lock);
    return p;
  }
  for(p = proc_head; p; p = p->next){
    acquire(&p->lock);
    if(p->state != UNUSED && p->pid == pid)
      return p;
    release(&p->lock);
  }
  return 0;
}

// p's attributes changed (caller holds p->lock): let the affected CPU decide
// again.  A running process yields at its next interrupt or syscall return.
static void
sched_attr_changed(struct proc *p)
{
  if(p->state == RUNNABLE)
    resched_hint(p);
  else if(p->state == RUNNING && p->last_cpu >= 0)
    kick_cpu(p->last_cpu);
}

int
sched_setscheduler(int pid, int policy, int priority)
{
  struct proc *p;

  if(policy == SCHED_OTHER){
    if(priority != 0)
      return -1;
  } else if(policy == SCHED_FIFO || policy == SCHED_RR){
    if(priority < SCHED_PRIO_MIN || priority > SCHED_PRIO_MAX)
      return -1;
  } else {
    return -1;
  }
  if((p = lock_proc_by_pid(pid)) == 0)
    return -1;
  p->policy = policy;
  p->rt_priority = priority;
  p->slice = timeslice(p);
  sched_attr_changed(p);
  release(&p->lock);
  return 0;
}

int
sched_setnice(int pid, int nice)
{
  struct proc *p;

  if(nice < NICE_MIN || nice > NICE_MAX)
    return -1;
  if((p = lock_proc_by_pid(pid)) == 0)
    return -1;
  p->nice = nice;
  if(p->policy == SCHED_OTHER && p->slice > timeslice(p))
    p->slice = timeslice(p);
  release(&p->lock);
  return 0;
}

int
sched_setaffinity(int pid, uint mask)
{
  struct proc *p;

  mask &= ALL_CPUS;
  if(mask == 0)
    return -1;
  if((p = lock_proc_by_pid(pid)) == 0)
    return -1;
  p->cpumask = mask;
  if(p->state == RUNNABLE)
    resched_hint(p);
  else if(p->state == RUNNING && p->last_cpu >= 0 &&
          !(mask & (1U << p->last_cpu)))
    kick_cpu(p->last_cpu);      // move it off a CPU it may no longer use
  release(&p->lock);
  return 0;
}

int
sched_getinfo(int pid, struct sched_info *info)
{
  struct proc *p;

  if((p = lock_proc_by_pid(pid)) == 0)
    return -1;
  info->pid = p->pid;
  info->policy = p->policy;
  info->priority = p->rt_priority;
  info->nice = p->nice;
  info->cpumask = p->cpumask;
  info->cpu = p->last_cpu;
  info->state = p->state;
  info->run_ticks = p->run_ticks;
  release(&p->lock);
  return 0;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->vm->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->vm->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc_head; p; p = p->next){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}

static char*
procputstr(char *p, char *end, char *s)
{
  while(*s && p < end)
    *p++ = *s++;
  return p;
}

static char*
procputpad(char *p, char *end, char *s, int width)
{
  int n = strlen(s);
  while(n++ < width && p < end)
    *p++ = ' ';
  return procputstr(p, end, s);
}

static char*
procputint(char *p, char *end, int value, int width)
{
  char digits[16], s[18];
  int n = 0, i = 0;
  uint v = value < 0 ? -value : value;
  do {
    digits[n++] = '0' + v % 10;
    v /= 10;
  } while(v && n < sizeof(digits));
  if(value < 0)
    s[i++] = '-';
  while(n)
    s[i++] = digits[--n];
  s[i] = 0;
  return procputpad(p, end, s, width);
}

// Format a process snapshot for a user program.  Unlike procdump(), this does
// not write the kernel console; the caller can send it through stdout/PTY.
int
proclist(char *buf, int size)
{
  static char *states[] = {
    [UNUSED] "unused", [USED] "used", [SLEEPING] "sleeping",
    [RUNNABLE] "runnable", [RUNNING] "running", [ZOMBIE] "zombie"
  };
  static char *policies[] = {
    [SCHED_OTHER] "OTHER", [SCHED_FIFO] "FIFO", [SCHED_RR] "RR"
  };
  static char hex[] = "0123456789abcdef";
  char *q = buf, *end = buf + size;

  q = procputstr(q, end,
                 "  TID  TGID CPU POLICY PRI  NI MASK     TIME STATE    NAME\n");
  for(struct proc *p = proc_head; p; p = p->next){
    acquire(&p->lock);
    if(p->state != UNUSED){
      char mask[3] = { hex[(p->cpumask >> 4) & 0xf], hex[p->cpumask & 0xf], 0 };
      uint ms = p->run_ticks * SCHED_TICK_MS;
      q = procputint(q, end, p->pid, 5);
      q = procputint(q, end, p->tgid, 6);
      if(p->last_cpu < 0)
        q = procputpad(q, end, "-", 4);
      else
        q = procputint(q, end, p->last_cpu, 4);
      q = procputpad(q, end, policies[p->policy], 7);
      q = procputint(q, end, p->rt_priority, 4);
      q = procputint(q, end, p->nice, 4);
      q = procputpad(q, end, mask[0] == '0' ? mask + 1 : mask, 5);
      q = procputint(q, end, ms / 1000, 6);
      q = procputstr(q, end, ".");
      q = procputstr(q, end, (char[]){ '0' + (ms / 100) % 10, '0' + (ms / 10) % 10, 0 });
      q = procputstr(q, end, " ");
      q = procputstr(q, end, states[p->state]);
      for(int n = strlen(states[p->state]); n < 9; n++)
        q = procputstr(q, end, " ");
      q = procputstr(q, end, p->name);
      q = procputstr(q, end, "\n");
    }
    release(&p->lock);
  }
  return q - buf;
}

// Print stable-enough snapshots of process page tables for diagnostics.
// pid == 0 selects every active process.
int
procvmdump(int pid)
{
  struct proc *p;
  int found = 0;

  kvmdump();
  printf("\n=== occupied TTBR0 page-table entries ===\n");
  printf("VA[38:30]=L1 VA[29:21]=L2 VA[20:12]=L3 VA[11:0]=offset\n");
  for(p = proc_head; p; p = p->next){
    acquire(&p->lock);
    if(p->state != UNUSED && p->vm != 0 && p->vm->pagetable != 0 &&
       (pid == 0 || p->pid == pid)){
      uvmdump(p->vm->pagetable, p->pid, p->name, "vmmap");
      found++;
    }
    release(&p->lock);
  }
  printf("=== end occupied TTBR0 page-table entries ===\n\n");
  return found ? 0 : -1;
}
