#include <kernel/types.h>
#include <kernel/param.h>
#include "memlayout.h"
#include "aarch64.h"
#include <kernel/spinlock.h>
#include <kernel/sleeplock.h>
#include <kernel/fs.h>
#include <kernel/file.h>
#include <kernel/proc.h>
#include <kernel/page.h>
#include <kernel/sched.h>
#include <kernel/camera.h>
#include <kernel/win32k.h>
#include <kernel/defs.h>
#include <kernel/mm.h>
#include <kernel/slab.h>
#include <uapi/sched.h>

struct cpu cpus[NCPU];
struct proc *proc_head;
static struct spinlock proc_table_lock;
static struct spinlock kernel_stack_lock;
static uint64 next_kstack_slot;
static struct kmem_cache proc_cache;
static struct kmem_cache vmspace_cache;

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);
static void threadexit(int status) __attribute__((noreturn));
static void reparent(struct proc *p);

#if PROC_SCHED_TRACE
static uint proc_sched_trace_seq;
static struct proc *proc_sched_last_task[NCPU];
static int proc_sched_last_pid[NCPU];
static struct context proc_sched_last_context[NCPU];
static struct trapframe proc_sched_last_trapframe[NCPU];
static int proc_sched_last_has_trapframe[NCPU];
#define PROC_SCHED_BT_MAX 12
static uint64 proc_sched_last_bt_fp[NCPU][PROC_SCHED_BT_MAX];
static uint64 proc_sched_last_bt_lr[NCPU][PROC_SCHED_BT_MAX];
static int proc_sched_last_nbt[NCPU];
// One-line "quiet" events: what the full trace hides.  A task re-picked by
// the CPU whose last visible task it already was prints nothing above; these
// lines show every such run, why each run ended, and every fork.
#ifndef PROC_SCHED_QUIET_LIMIT
#define PROC_SCHED_QUIET_LIMIT 512
#endif
static uint proc_sched_quiet_seq;
static int proc_sched_repeat[NCPU];
// Kernel threads (kthread_fn != 0) stay transparent to the full trace in
// PID=-1 mode, but with PROC_SCHED_TRACE_KTHREAD they get their own one-line
// kthread-in / kthread-out events, numbered in the same sched-q sequence so
// they interleave with the user-task lines.  Separate budget, because a busy
// worker must not starve the user-task lines.
#ifndef PROC_SCHED_TRACE_KTHREAD
#define PROC_SCHED_TRACE_KTHREAD 1
#endif
#ifndef PROC_SCHED_KTHREAD_LIMIT
#define PROC_SCHED_KTHREAD_LIMIT 256
#endif
static uint proc_sched_kthread_seq;
// PROC_SCHED_TRACE_ALL: every dispatch and every switch-out of every task
// (user processes and kernel threads alike) prints a full record.  The
// switch-out record is taken right after swtch() returns, so its context and
// backtrace are the live values, not a per-CPU snapshot.  All trace output is
// serialized by proc_sched_print_lock, so records from different CPUs no
// longer interleave line by line.
#ifndef PROC_SCHED_TRACE_ALL
#define PROC_SCHED_TRACE_ALL 0
#endif
static struct spinlock proc_sched_print_lock;
#define PROC_SCHED_PRINTF(...) do { \
    acquire(&proc_sched_print_lock); printf(__VA_ARGS__); \
    release(&proc_sched_print_lock); } while(0)

// Walk the AArch64 frame records {saved x29, saved x30} that the kernel's
// -fno-omit-frame-pointer prologues leave on p's kernel stack, starting at
// the fp saved in p->context (sched()'s own frame).  Only records strictly
// inside the stack page and below the trapframe are followed, so the walk
// stops at usertrap()/userirq(), whose caller is the EL0 trap entry.
static int
proc_sched_backtrace(struct proc *p, uint64 fp, uint64 *fps, uint64 *lrs)
{
  uint64 lo = p->kstack, hi = p->kstack + PGSIZE;
  int n = 0;

  if(p->kstack == 0 || p->kstack_pa == 0)
    return 0;
  if(p->trapframe && (uint64)p->trapframe > lo && (uint64)p->trapframe <= hi)
    hi = (uint64)p->trapframe;
  while(n < PROC_SCHED_BT_MAX && fp >= lo && fp + 16 <= hi && (fp & 0xf) == 0){
    uint64 *rec = (uint64 *)fp;
    fps[n] = fp;
    lrs[n++] = rec[1];
    if(rec[0] <= fp)
      break;
    fp = rec[0];
  }
  return n;
}

static uint
proc_sched_atomic_inc(uint *addr)
{
  uint old, next, failed;

  // The freestanding kernel does not link libgcc's out-of-line atomics.
  // Use the same AArch64 exclusive-monitor pattern as the spinlock code.
  asm volatile(
    "1: ldaxr %w0, [%3]\n"
    "   add %w1, %w0, #1\n"
    "   stlxr %w2, %w1, [%3]\n"
    "   cbnz %w2, 1b\n"
    : "=&r" (old), "=&r" (next), "=&r" (failed)
    : "r" (addr)
    : "memory");
  return old;
}

static uint
proc_sched_trace_next(void)
{
  return proc_sched_atomic_inc(&proc_sched_trace_seq);
}

// Returns -1 when the quiet budget is used up.
static int
proc_sched_quiet_next(void)
{
  uint seq = proc_sched_atomic_inc(&proc_sched_quiet_seq);

  if(PROC_SCHED_QUIET_LIMIT != 0 && seq >= PROC_SCHED_QUIET_LIMIT)
    return -1;
  return (int)seq;
}

// sched-q number for a kernel-thread event, or -1 when off / over budget.
static int
proc_sched_kthread_next(void)
{
  if(PROC_SCHED_TRACE_ALL || !PROC_SCHED_TRACE_KTHREAD ||
     PROC_SCHED_TRACE_PID >= 0)
    return -1;
  if(PROC_SCHED_KTHREAD_LIMIT != 0 &&
     proc_sched_atomic_inc(&proc_sched_kthread_seq) >= PROC_SCHED_KTHREAD_LIMIT)
    return -1;
  return proc_sched_quiet_next();
}

static const char*
procstate_name(enum procstate state)
{
  switch(state){
  case UNUSED:   return "UNUSED";
  case USED:     return "USED";
  case SLEEPING: return "SLEEPING";
  case RUNNABLE: return "RUNNABLE";
  case RUNNING:  return "RUNNING";
  case ZOMBIE:   return "ZOMBIE";
  }
  return "?";
}

static int
proc_sched_trace_selected(struct proc *p)
{
  if(p == 0)
    return 0;
  if(PROC_SCHED_TRACE_ALL)
    return 1;
  if(PROC_SCHED_TRACE_PID > 0)
    return p->pid == PROC_SCHED_TRACE_PID;
  if(PROC_SCHED_TRACE_PID < 0)
    return p->kthread_fn == 0;
  return 1;
}

// Print the objects that participate in one scheduling transition, without
// walking the process page table.  At scheduler:saved-out, p->context has just
// been written by swtch().  At scheduler:dispatch-load it is about to be
// loaded.  At sched:restored-in those values are live in the CPU again.
static void
proc_sched_trace(struct cpu *c, struct proc *p, char *event, char *context_role,
                 struct context *saved_ctx, struct trapframe *saved_tf)
{
  struct context *ctx;
  struct trapframe *tf;
  uint seq;
  uint64 current_sp;

  if(!proc_sched_trace_selected(p))
    return;
  // Take the print lock before numbering, so record numbers appear in order.
  acquire(&proc_sched_print_lock);
  seq = proc_sched_trace_next();
  if(PROC_SCHED_TRACE_LIMIT != 0 && seq >= PROC_SCHED_TRACE_LIMIT){
    release(&proc_sched_print_lock);
    return;
  }

  ctx = saved_ctx ? saved_ctx : &p->context;
  tf = saved_tf ? saved_tf : p->trapframe;
  asm volatile("mov %0, sp" : "=r" (current_sp));

  printf("\n--- sched-trace[%d] cpu=%d %s (%s) ---\n",
         seq, (int)(c - cpus), event, context_role);
  printf("proc=%p pid=%d tgid=%d name=%s state=%s(%d) parent=%p\n",
         (uint64)p, p->pid, p->tgid, p->name, procstate_name(p->state),
         p->state, (uint64)p->parent);
  printf(" task: thread=%d chan=%p killed=%d xstate=%d signals=%p"
         " sid=%d pgid=%d ctty=%d\n",
         p->is_thread, (uint64)p->chan, p->killed, p->xstate,
         (uint64)p->signals_pending, p->sid, p->pgid, p->ctty);
  printf(" sched: policy=%d rtprio=%d nice=%d cpumask=%p slice=%d"
         " rq_seq=%p last_cpu=%d run_ticks=%p\n",
         p->policy, p->rt_priority, p->nice, (uint64)p->cpumask, p->slice,
         p->rq_seq, p->last_cpu, p->run_ticks);
  printf(" refs: vm=%p pagetable=%p sz=%p files=%p fs=%p"
         " kthread_fn=%p arg=%p\n",
         (uint64)p->vm,
         p->vm ? (uint64)p->vm->pagetable : 0,
         p->vm ? p->vm->sz : 0,
         (uint64)p->files, (uint64)p->fs,
         (uint64)p->kthread_fn, (uint64)p->kthread_arg);
  printf(" kstack: base=%p top=%p pa=%p slot=%p current-sp=%p %s\n",
         p->kstack, p->kstack + PGSIZE, p->kstack_pa, p->kstack_slot,
         current_sp,
         current_sp >= p->kstack && current_sp < p->kstack + PGSIZE ?
           "(on this process stack)" : "(on scheduler/other stack)");

  printf(" context=%p: sp=%p x18=%p x19=%p x20=%p x21=%p x22=%p x23=%p\n",
         (uint64)ctx, ctx->sp, ctx->x18, ctx->x19, ctx->x20,
         ctx->x21, ctx->x22, ctx->x23);
  printf("              x24=%p x25=%p x26=%p x27=%p x28=%p"
         " x29(fp)=%p x30(lr)=%p\n",
         ctx->x24, ctx->x25, ctx->x26, ctx->x27, ctx->x28,
         ctx->x29, ctx->x30);
  // The lines above are wider than an 80-column serial console; repeat the
  // frame pointer and return address on a short line of their own.
  printf(" ctx: x29(fp)=%p x30(lr)=%p\n", ctx->x29, ctx->x30);
  {
    uint64 bt_fp[PROC_SCHED_BT_MAX], bt_lr[PROC_SCHED_BT_MAX];
    uint64 *fps = bt_fp, *lrs = bt_lr;
    int nbt;

    if(saved_ctx){
      int cpu = (int)(c - cpus);
      fps = proc_sched_last_bt_fp[cpu];
      lrs = proc_sched_last_bt_lr[cpu];
      nbt = proc_sched_last_nbt[cpu];
    } else {
      nbt = proc_sched_backtrace(p, ctx->x29, bt_fp, bt_lr);
    }
    for(int i = 0; i < nbt; i++)
      printf("  bt#%d fp=%p lr=%p\n", i, fps[i], lrs[i]);
  }

  printf(" trapframe=%p", (uint64)tf);
  if(tf == 0){
    printf(" (kernel thread: no EL0 return frame)\n");
  } else {
    printf("%s: elr(user-pc)=%p spsr=%p sp_el0=%p"
           " x29(fp)=%p x30(lr)=%p\n",
           p->kthread_fn ? " (allocated but unused by kernel thread)" : "",
           tf->elr, tf->spsr, tf->sp, tf->x29, tf->x30);
    printf("  x0=%p x1=%p x2=%p x3=%p x4=%p x5=%p x6=%p x7=%p\n",
           tf->x0, tf->x1, tf->x2, tf->x3,
           tf->x4, tf->x5, tf->x6, tf->x7);
    printf("  x8=%p x9=%p x10=%p x11=%p x12=%p x13=%p x14=%p x15=%p\n",
           tf->x8, tf->x9, tf->x10, tf->x11,
           tf->x12, tf->x13, tf->x14, tf->x15);
    printf("  x16=%p x17=%p x18=%p x19=%p x20=%p x21=%p x22=%p x23=%p\n",
           tf->x16, tf->x17, tf->x18, tf->x19,
           tf->x20, tf->x21, tf->x22, tf->x23);
    printf("  x24=%p x25=%p x26=%p x27=%p x28=%p\n",
           tf->x24, tf->x25, tf->x26, tf->x27, tf->x28);
  }
  printf("--- end sched-trace[%d] ---\n", seq);
  release(&proc_sched_print_lock);
}

// Report only a real transition between two different visible tasks.  With
// PID=-1, kernel threads are deliberately transparent: user A -> kworker ->
// user A is not reported, while user A -> kworker -> user B is one A -> B
// transition.  proc nodes are permanent; the saved pid prevents a recycled
// slot from being mistaken for the old task.
static void
proc_sched_trace_dispatch(struct cpu *c, struct proc *next)
{
  int cpu = (int)(c - cpus);
  struct proc *prev;
  int prev_pid;

  if(PROC_SCHED_TRACE_ALL){
    char *ev;
    prev = proc_sched_last_task[cpu];
    prev_pid = proc_sched_last_pid[cpu];
    ev = prev == 0 ? "scheduler:first-dispatch" :
         (prev == next && prev_pid == next->pid) ?
           "scheduler:again-in" : "scheduler:switch-in";
    proc_sched_last_task[cpu] = next;
    proc_sched_last_pid[cpu] = next->pid;
    proc_sched_trace(c, next, ev, next->kthread_fn ?
                     "kernel thread context will be loaded by swtch" :
                     "user task context will be loaded by swtch", 0, 0);
    return;
  }

  // In user-tree mode kernel workers neither print a full record nor replace
  // last_task; they only get a one-line kthread-in event.
  if(PROC_SCHED_TRACE_PID < 0 && next->kthread_fn != 0){
    int q = proc_sched_kthread_next();
    if(q >= 0)
      PROC_SCHED_PRINTF("sched-q#%d cpu=%d kthread-in pid=%d name=%s slice=%d"
             " sp=%p (cpu's last user task pid=%d)\n", q, cpu, next->pid,
             next->name, next->slice, next->context.sp,
             proc_sched_last_pid[cpu]);
    return;
  }

  prev = proc_sched_last_task[cpu];
  prev_pid = proc_sched_last_pid[cpu];
  if(prev == next && prev_pid == next->pid){
    int q;
    proc_sched_repeat[cpu]++;
    if(proc_sched_trace_selected(next) && (q = proc_sched_quiet_next()) >= 0)
      PROC_SCHED_PRINTF("sched-q#%d cpu=%d again pid=%d name=%s repeat=%d slice=%d"
             " sp=%p\n", q, cpu, next->pid, next->name,
             proc_sched_repeat[cpu], next->slice, next->context.sp);
    return;
  }
  if(prev){
    int q;
    if(proc_sched_trace_selected(next) && (q = proc_sched_quiet_next()) >= 0)
      PROC_SCHED_PRINTF("sched-q#%d cpu=%d visible-change pid=%d -> pid=%d after"
             " %d hidden repeat(s) (before sched-trace[%d])\n", q, cpu,
             prev_pid, next->pid, proc_sched_repeat[cpu],
             proc_sched_trace_seq);
  }
  proc_sched_repeat[cpu] = 0;

  proc_sched_last_task[cpu] = next;
  proc_sched_last_pid[cpu] = next->pid;

  if(prev && prev->pid == prev_pid)
    proc_sched_trace(c, prev, "scheduler:switch-out",
                     "saved context of previous visible task",
                     &proc_sched_last_context[cpu],
                     proc_sched_last_has_trapframe[cpu] ?
                       &proc_sched_last_trapframe[cpu] : 0);
  proc_sched_trace(c, next,
                   prev ? "scheduler:switch-in" : "scheduler:first-dispatch",
                   "context will be loaded by swtch", 0, 0);
}

// Called immediately after swtch() has returned to scheduler(), while p->lock
// is still held.  Keep the exact saved context/trapframe so a migration to
// another CPU cannot change the values before the next visible dispatch.
static void
proc_sched_trace_save_outgoing(struct cpu *c, struct proc *p)
{
  int cpu = (int)(c - cpus);

  int q;

  if(PROC_SCHED_TRACE_ALL){
    // Live: swtch() has just written p->context and p's stack is intact.
    (void)cpu; (void)q;
    proc_sched_trace(c, p, "scheduler:switch-out",
                     p->state == RUNNABLE ? "live context, preempted/yield" :
                     p->state == SLEEPING ? "live context, went to sleep" :
                     p->state == ZOMBIE ? "live context, exited" :
                     "live context", 0, 0);
    return;
  }

  if(PROC_SCHED_TRACE_PID < 0 && p->kthread_fn != 0){
    if((q = proc_sched_kthread_next()) >= 0){
      // Three callers above sched(): e.g. sleep <- worker loop <- kthread
      // entry, so the line says where the worker parked.
      uint64 fps[PROC_SCHED_BT_MAX], lrs[PROC_SCHED_BT_MAX];
      int n = proc_sched_backtrace(p, p->context.x29, fps, lrs);
      PROC_SCHED_PRINTF("sched-q#%d cpu=%d kthread-out pid=%d name=%s -> %s chan=%p"
             " slice=%d via0.lr=%p via1.lr=%p via2.lr=%p\n", q, cpu,
             p->pid, p->name, procstate_name(p->state), p->chan, p->slice,
             n > 0 ? lrs[0] : 0, n > 1 ? lrs[1] : 0, n > 2 ? lrs[2] : 0);
    }
    return;
  }
  if(proc_sched_trace_selected(p) && (q = proc_sched_quiet_next()) >= 0)
    PROC_SCHED_PRINTF("sched-q#%d cpu=%d out pid=%d name=%s -> %s chan=%p slice=%d"
           " sp=%p lr=%p\n", q, cpu, p->pid, p->name,
           procstate_name(p->state), p->chan, p->slice, p->context.sp,
           p->context.x30);
  if(proc_sched_last_task[cpu] != p || proc_sched_last_pid[cpu] != p->pid)
    return;
  proc_sched_last_context[cpu] = p->context;
  proc_sched_last_has_trapframe[cpu] = p->trapframe != 0;
  if(p->trapframe)
    proc_sched_last_trapframe[cpu] = *p->trapframe;
  // Walk now: once p runs again (possibly on another CPU) its stack changes.
  proc_sched_last_nbt[cpu] =
    proc_sched_backtrace(p, p->context.x29, proc_sched_last_bt_fp[cpu],
                         proc_sched_last_bt_lr[cpu]);
}

// SLEEPING -> RUNNABLE: who woke p, on which CPU, from where.  Numbered in
// the same sequence as the full records, so it sorts between them.  The
// waker is the process current on this CPU (-1 = none: an interrupt that hit
// the idle scheduler loop).  Interrupts are always masked here (p->lock is
// held), so the from.lr frame tells task context from interrupt context.
static void
proc_sched_trace_wake(struct proc *p, uint64 lr0, uint64 lr1)
{
  struct cpu *c = mycpu();
  struct proc *w = c->proc;
  uint seq;

  if(!(PROC_SCHED_TRACE_ALL || proc_sched_trace_selected(p)))
    return;
  acquire(&proc_sched_print_lock);
  seq = proc_sched_trace_next();
  if(PROC_SCHED_TRACE_LIMIT == 0 || seq < PROC_SCHED_TRACE_LIMIT)
    printf("sched-wake[%d] cpu=%d wake pid=%d name=%s chan=%p"
           " by pid=%d name=%s caller.lr=%p from.lr=%p\n",
           seq, (int)(c - cpus), p->pid, p->name, (uint64)p->chan,
           w ? w->pid : -1, w ? w->name : "-", lr0, lr1);
  release(&proc_sched_print_lock);
}
#endif

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

extern pagetable_t kernel_pagetable;

static struct vmspace*
vmspace_alloc(void)
{
  struct vmspace *vm = kmem_cache_alloc(&vmspace_cache);
  if(vm == 0)
    return 0;
  initlock(&vm->lock, "vmspace");
  init_rwsem(&vm->mmap_lock, "mmap_lock");
  vm->pagetable = uvmcreate();
  if(vm->pagetable == 0){
    kmem_cache_free(&vmspace_cache, vm);
    return 0;
  }
  vm->refcount = 1;
  vm->users = 1;
  return vm;
}

// Wrap an already built page table in a fresh private vmspace.  exec() uses
// this when the caller shares its address space with a CLONE_VM sibling (a
// vfork parent), so the new image must not replace the shared one.
struct vmspace*
vmspace_wrap(pagetable_t pagetable, uint64 sz)
{
  struct vmspace *vm = kmem_cache_alloc(&vmspace_cache);
  if(vm == 0)
    return 0;
  initlock(&vm->lock, "vmspace");
  init_rwsem(&vm->mmap_lock, "mmap_lock");
  vm->pagetable = pagetable;
  vm->sz = sz;
  vm->start_brk = sz;
  vm->brk = sz;
  vm->refcount = 1;
  vm->users = 1;
  return vm;
}

// Drop a wrapper that exec() prepared but did not install.  Its page table is
// still owned by exec's error path, so this deliberately frees only the
// small vmspace object.
void
vmspace_discard(struct vmspace *vm)
{
  if(vm)
    kmem_cache_free(&vmspace_cache, vm);
}

// A task stops running on vm (exit, thread exit, or exec of a vfork
// child), as Linux mmput(): the last one tears down the mmap() regions.
// Unlike vmspace_put() this may sleep, so it runs before the task becomes a
// zombie; refcount keeps the page table alive until freeproc().
void
vmspace_leave(struct vmspace *vm)
{
  int last;

  acquire(&vm->lock);
  if(vm->users < 1)
    panic("vmspace_leave");
  last = --vm->users == 0;
  release(&vm->lock);
  if(last)
    exit_mmap(vm);
}

void
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
    vm->start_brk = 0;
    vm->brk = 0;
  }
  release(&vm->lock);
  if(last){
    uvmfree(pagetable, sz);
    kmem_cache_free(&vmspace_cache, vm);
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
#if PROC_SCHED_TRACE
static void proc_sched_trace_wake(struct proc *p, uint64 lr0, uint64 lr1);
// Not inlined while tracing, so __builtin_return_address(0) is the real
// caller (wakeup / kill / fork / ...) and (1) is the one above it.
__attribute__((noinline))
#endif
static void
make_runnable(struct proc *p, int at_head)
{
#if PROC_SCHED_TRACE
  if(p->state == SLEEPING){
    // AArch64 frame records {fp, lr}: ours gives the caller, the caller's
    // record gives the function above it.
    uint64 *fr = (uint64 *)__builtin_frame_address(0);
    uint64 *up = (uint64 *)fr[0];
    proc_sched_trace_wake(p, fr[1], up ? up[1] : 0);
  }
#endif
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
  p->vm_trace_exec_printed = 0;
  p->vm_trace_syscall_printed = 0;
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
  if(kmem_cache_init(&proc_cache, "proc_cache", sizeof(struct proc), 16) < 0 ||
     kmem_cache_init(&vmspace_cache, "vmspace_cache",
                     sizeof(struct vmspace), 16) < 0)
    panic("proc slab caches");
  initlock(&proc_table_lock, "proc_table");
  initlock(&kernel_stack_lock, "kernel_stack");
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
#if PROC_SCHED_TRACE
  initlock(&proc_sched_print_lock, "sched_trace");
#endif
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
  struct proc *p;
  uint64 slot, va;

  // Slots are reused once UNUSED, so this bounds live tasks (EAGAIN).
  if(next_kstack_slot >= NPROC_MAX)
    return 0;
  if((p = kmem_cache_alloc(&proc_cache)) == 0)
    return 0;
  if(sizeof(*p) > PGSIZE)
    panic("struct proc too large");
  initlock(&p->lock, "proc");

  slot = next_kstack_slot++;
  va = KSTACK(slot);
  // Keep dynamically growing stacks in the high sparse KSTACK region and
  // never let an arithmetic wrap collide with the kernel direct map.
  if(va < KERNBASE + (1ULL << 37)){
    kmem_cache_free(&proc_cache, p);
    return 0;
  }
  p->kstack = va;
  p->kstack_slot = slot;
  if(alloc_kstack(p) < 0){
    kmem_cache_free(&proc_cache, p);
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
  // kalloc() poisons free pages with 0x05.  A fresh user task must not return
  // to EL0 with those bytes in registers that userinit() does not overwrite.
  memset(p->trapframe, 0, sizeof(*p->trapframe));

  // An empty user address space. clone() replaces this reference with its
  // caller's vmspace; fork() keeps it and copies pages into it.
  p->vm = vmspace_alloc();
  if(p->vm == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Likewise an empty descriptor table and an fs_struct with no cwd.
  // do_clone() replaces them with shared or copied ones.
  p->files = files_alloc();
  p->fs = fs_alloc();
  if(p->files == 0 || p->fs == 0){
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
  // exit() already dropped live tables; only allocproc's or do_clone()'s
  // empty ones reach here, and putting those never sleeps.
  files_put(p->files);
  p->files = 0;
  fs_put(p->fs);
  p->fs = 0;
  p->nfpin = 0;
  p->vfork_pending = 0;
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
  p->vm->start_brk = PGSIZE;
  p->vm->brk = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->elr = 0;      // user program counter
  p->trapframe->spsr = 0;     // switch to EL0
  p->trapframe->sp = PGSIZE;  // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  // p->fs->cwd is set by prepare_namespace() in forkret(); cwdpath is "/".

  make_runnable(p, 0);

#if PROC_SCHED_TRACE
  proc_sched_trace(mycpu(), p, "userinit:runnable",
                   "new context: sp=trapframe, x30=forkret", 0, 0);
#endif

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n, uint64 *oldbrk)
{
  uint64 old, new, first, end;
  struct proc *p = myproc();
  struct vmspace *vm = p->vm;

  // Linux serializes brk changes against page faults and mmap/munmap with
  // mmap_lock.  This also makes sbrk's returned old break atomic for threads
  // sharing one vmspace.
  down_write(&vm->mmap_lock);
  acquire(&vm->lock);
  old = vm->brk;
  if(oldbrk)
    *oldbrk = old;
  if(n > 0){
    new = old + (uint64)n;
    // Reserve only virtual space.  The first read/write of each page enters
    // handle_user_mm_fault(), which allocates and zeroes the physical page.
    if(new < old || PGROUNDUP(new) < new ||
       PGROUNDUP(new) > mmap_lowest(vm)){
      release(&vm->lock);
      up_write(&vm->mmap_lock);
      return -1;
    }
    vm->brk = vm->sz = new;
    // A CPU may have cached a negative translation for a page that just
    // became part of the heap.
    flush_tlb();
  } else if(n < 0){
    uint64 shrink = (uint64)(-(long)n);
    if(shrink > old - vm->start_brk){
      release(&vm->lock);
      up_write(&vm->mmap_lock);
      return -1;
    }
    new = old - shrink;
    first = PGROUNDUP(new);
    end = PGROUNDUP(old);

    // A shared page table can be active on another Cortex-A53.  Clear PTEs,
    // broadcast the invalidation, and only then recycle physical pages.  A
    // lazy heap legitimately contains holes, so absent PTEs are skipped.
    while(first < end){
      uint64 pa[64];
      int count = 0;
      while(first < end && count < (int)NELEM(pa)){
        pte_t *pte = walk(vm->pagetable, first, 0);
        if(pte == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_AF) == 0){
          first += PGSIZE;
          continue;
        }
        // Shared libc.so text belongs to exec's cache: unmap, never free.
        if((*pte & PTE_SHARED) == 0)
          pa[count++] = PTE2PA(*pte);
        *pte = 0;
        first += PGSIZE;
      }
      // VMALLE1IS reaches every PE in the inner-shareable domain.
      flush_tlb();
      for(int i = 0; i < count; i++)
        put_page(virt_to_page(P2V(pa[i])));   // may be COW-shared
    }
    vm->brk = vm->sz = new;
  } else {
    new = old;
  }
  release(&vm->lock);
  up_write(&vm->mmap_lock);
  return 0;
}

// clone flags this kernel implements.  CLONE_SIGHAND is accepted for the
// Linux rules below; signal dispositions are not per task yet.
#define CLONE_SUPPORTED (CSIGNAL | CLONE_VM | CLONE_FS | CLONE_FILES | \
                         CLONE_SIGHAND | CLONE_VFORK | CLONE_THREAD | \
                         CLONE_PARENT_SETTID | CLONE_CHILD_SETTID)

// Common implementation of fork, the thread library's clone and the Linux
// style clone, after Linux copy_process().  Every child gets its own proc
// slot, kernel stack, context and trapframe; flags select which of the
// address space (CLONE_VM), descriptor table (CLONE_FILES) and cwd
// (CLONE_FS) are shared rather than copied.
//
// stack, if non-zero, becomes the child's sp.  entry, if non-zero, starts
// the child at entry(arg) instead of returning 0 from the system call; the
// thread-library SYS_clone ABI uses this.  CLONE_VFORK suspends the caller
// until the child calls exec() or exits.
int
do_clone(uint64 flags, uint64 stack, uint64 ptid, uint64 ctid,
         uint64 entry, uint64 arg)
{
  int id;
  struct proc *np;
  struct proc *p = myproc();
  struct vmspace *vm = p->vm;
  struct files_struct *files;
  struct fs_struct *fs;
  int exit_signal = flags & CSIGNAL;

  // Linux's consistency rules: a thread shares signal handlers, which in
  // turn requires a shared address space.
  if(flags & ~(uint64)CLONE_SUPPORTED)
    return -1;
  if((flags & CLONE_THREAD) && !(flags & CLONE_SIGHAND))
    return -1;
  if((flags & CLONE_SIGHAND) && !(flags & CLONE_VM))
    return -1;
  if(exit_signal != 0 && exit_signal != SIGCHLD)
    return -1;
  // Two runnable tasks cannot use one user stack.  A vfork child may,
  // because its parent stays suspended until the child execs or exits.
  if((flags & CLONE_VM) && !(flags & CLONE_VFORK) && stack == 0)
    return -1;
  if(stack != 0 && (stack < 16 || (stack & 15) != 0))
    return -1;

  if(flags & CLONE_FILES){
    files_get(p->files);
    files = p->files;
  } else if((files = files_dup(p->files)) == 0){
    return -1;
  }
  if(flags & CLONE_FS){
    fs_get(p->fs);
    fs = p->fs;
  } else if((fs = fs_copy(p->fs)) == 0){
    files_put(files);
    return -1;
  }

  // A malloc-backed thread stack may be in an untouched lazy-brk page (or
  // in a large anonymous mmap).  Materialize its top page before validating
  // it; swtch cannot recover from discovering the missing user stack later.
  if((flags & CLONE_VM) && stack != 0 &&
     fault_in_user_range(stack - 1, 1, 1) < 0)
    goto bad;

  // fork copies the VMAs: keep mmap/munmap out until the copy is done.  The
  // sleepable lock must be taken before allocproc() returns np->lock held.
  if(!(flags & CLONE_VM))
    down_read(&vm->mmap_lock);
  if((np = allocproc()) == 0){
    if(!(flags & CLONE_VM))
      up_read(&vm->mmap_lock);
    goto bad;
  }

  if(flags & CLONE_VM){
    acquire(&vm->lock);
    if(vm->execing ||
       // ld.so maps libc in the mmap window, so a pthread entry is not
       // necessarily below vm->sz.  Its code was populated by the loader;
       // validate the actual user mapping instead of the legacy low-image
       // bound.
       (entry != 0 && walkaddr(vm->pagetable, entry) == 0) ||
       (stack != 0 && walkaddr(vm->pagetable, stack - 1) == 0)){
      release(&vm->lock);
      freeproc(np);
      release(&np->lock);
      goto bad;
    }
    vm->refcount++;
    vm->users++;
    release(&vm->lock);

    // Replace allocproc's private empty vmspace with the caller's vmspace.
    vmspace_put(np->vm);
    np->vm = vm;
  } else {
    // fork: retain allocproc's private vmspace and populate it from parent.
    acquire(&vm->lock);
    if(uvmcopy(vm->pagetable, np->vm->pagetable, vm->sz) < 0){
      flush_tlb();
      release(&vm->lock);
      up_read(&vm->mmap_lock);
      freeproc(np);
      release(&np->lock);
      goto bad;
    }
    np->vm->sz = vm->sz;
    np->vm->start_brk = vm->start_brk;
    np->vm->brk = vm->brk;
    if(dup_mmap(vm, np->vm) < 0){
      flush_tlb();      // parent PTEs may already be read-only + COW
      release(&vm->lock);
      up_read(&vm->mmap_lock);
      mmap_abort(np->vm);
      freeproc(np);
      release(&np->lock);
      goto bad;
    }
    // uvmcopy()/dup_mmap() turned the parent's writable PTEs read-only;
    // threads of the parent may hold the old translations on other CPUs.
    flush_tlb();
    release(&vm->lock);
    up_read(&vm->mmap_lock);
  }
  if(flags & CLONE_THREAD){
    np->is_thread = 1;
    np->tgid = p->tgid;
  } else {
    np->tgid = np->pid;
  }

  // Begin with the caller's saved user register image, then select the user
  // return point required by fork or thread creation.
  *np->trapframe = *p->trapframe;
  if(entry != 0){
    np->trapframe->elr = entry;
    np->trapframe->x0 = arg;
    np->trapframe->x30 = 0;
  } else {
    // fork/clone return zero in the child; the parent receives the child TID.
    np->trapframe->x0 = 0;
  }
  if(stack != 0)
    np->trapframe->sp = stack;

  // allocproc's empty tables are released without sleeping.
  files_put(np->files);
  np->files = files;
  fs_put(np->fs);
  np->fs = fs;
  np->sid = p->sid;
  np->pgid = p->pgid;
  np->ctty = p->ctty;
  safestrcpy(np->name, p->name, sizeof(np->name));

  // A child task inherits policy, priority, nice and affinity.
  acquire(&p->lock);
  np->policy = p->policy;
  np->rt_priority = p->rt_priority;
  np->nice = p->nice;
  np->cpumask = p->cpumask;
  release(&p->lock);
  np->slice = timeslice(np);

  id = np->pid;
  release(&np->lock);

  // The child has not run yet, so both TIDs are visible before it starts.
  // As in Linux, a bad pointer here is ignored rather than undoing clone.
  if(flags & CLONE_PARENT_SETTID)
    copyout(p->vm->pagetable, ptid, (char *)&id, sizeof(id));
  if(flags & CLONE_CHILD_SETTID)
    copyout(np->vm->pagetable, ctid, (char *)&id, sizeof(id));

  acquire(&wait_lock);
  np->parent = p;
  np->vfork_pending = (flags & CLONE_VFORK) != 0;
  release(&wait_lock);

  acquire(&np->lock);
  make_runnable(np, 0);
  release(&np->lock);
#if PROC_SCHED_TRACE
  {
    int q;
    if(proc_sched_trace_selected(p) && (q = proc_sched_quiet_next()) >= 0)
      PROC_SCHED_PRINTF("sched-q#%d cpu=%d fork parent pid=%d name=%s -> child pid=%d"
             " proc=%p kstack=%p flags=%p\n", q, cpuid(), p->pid, p->name,
             np->pid, np, np->kstack, flags);
  }
#endif

  // vfork: sleep until vfork_done() reports the child's exec or exit.  The
  // proc slot cannot be recycled meanwhile: only this parent reaps it.
  if(flags & CLONE_VFORK){
    acquire(&wait_lock);
    while(np->vfork_pending && !p->killed)
      sleep(&np->vfork_pending, &wait_lock);
    release(&wait_lock);
  }
  return id;

bad:
  files_put(files);
  fs_put(fs);
  return -1;
}

// Release a CLONE_VFORK parent: the child has exec'd a new image or is
// exiting, so it no longer borrows the parent's address space.
void
vfork_done(struct proc *p)
{
  acquire(&wait_lock);
  if(p->vfork_pending){
    p->vfork_pending = 0;
    wakeup(&p->vfork_pending);
  }
  release(&wait_lock);
}

// True when no other live task belongs to p's thread group.  Only a member
// of the group can add one, so the answer stays valid while p is busy.
int
thread_group_alone(struct proc *p)
{
  int alone = !p->is_thread;

  acquire(&wait_lock);
  for(struct proc *t = proc_head; t && alone; t = t->next){
    if(t == p)
      continue;
    acquire(&t->lock);
    if(t->is_thread && t->tgid == p->tgid && t->state != UNUSED &&
       t->state != ZOMBIE)
      alone = 0;
    release(&t->lock);
  }
  release(&wait_lock);
  return alone;
}

// Create a new process with a private copy of the caller's address space,
// descriptor table and cwd.
int
fork(void)
{
  return do_clone(SIGCHLD, 0, 0, 0, 0, 0);
}

// The thread library's fixed SYS_clone ABI: a POSIX thread in the caller's
// thread group, sharing address space, descriptor table and cwd, as
// pthread_create() asks of Linux.  entry receives arg in x0 and must
// terminate with texit(); the user library supplies a wrapper that
// guarantees this when the thread function returns.
int
threadclone(uint64 entry, uint64 arg, uint64 stack_top)
{
  if(entry == 0 || stack_top == 0)
    return -1;
  return do_clone(CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
                  CLONE_THREAD, stack_top, 0, 0, entry, arg);
}

static void
threadexit(int status)
{
  struct proc *p = myproc();

  vfork_done(p);
  net_udp_closeproc(p->pid);
  vmspace_leave(p->vm);
  // Drop this thread's references; the last sharer closes the files.
  fdput_all(p);
  files_put(p->files);
  p->files = 0;
  fs_put(p->fs);
  p->fs = 0;

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

  // A vfork child stops borrowing its parent's address space here.
  vfork_done(p);

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
  win32k_exit(p->tgid);         // its windows and message queue

  // The thread group is gone.  If no CLONE_VM sharer (a vfork parent)
  // remains either, write dirty MAP_SHARED pages back and drop the
  // mappings' file references while this process can still sleep and do I/O.
  vmspace_leave(p->vm);

  // Drop the descriptor table and cwd.  Open files are closed and cwd is
  // released only when no CLONE_FILES / CLONE_FS sharer remains.
  fdput_all(p);
  files_put(p->files);
  p->files = 0;
  fs_put(p->fs);
  p->fs = 0;

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

// Wait for a child to exit, like Linux waitpid(2).
//   pid > 0 : reap only that child; if it isn't ours, fail with -1 (ECHILD).
//   pid <= 0: reap any child, exactly like wait().
//   WNOHANG : if a matching child exists but none has exited yet, return 0
//             instead of blocking.
// Returns the reaped pid, 0 for a WNOHANG miss, or -1 on error.
#define WNOHANG 1
int
waitpid(int pid, uint64 addr, int options)
{
  struct proc *np;
  int havekids, rpid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan the process table for an exited child we are responsible for.
    havekids = 0;
    for(np = proc_head; np; np = np->next){
      if(np->parent != p || np->is_thread)
        continue;
      if(pid > 0 && np->pid != pid)
        continue;
      // A matching child exists, so a blocking wait must keep waiting
      // even while it is still running.
      havekids = 1;
      acquire(&np->lock);
      if(np->state == ZOMBIE){
        rpid = np->pid;
        if(addr != 0 && copyout(p->vm->pagetable, addr, (char *)&np->xstate,
                                sizeof(np->xstate)) < 0) {
          release(&np->lock);
          release(&wait_lock);
          return -1;
        }
        freeproc(np);
        release(&np->lock);
        release(&wait_lock);
        return rpid;
      }
      release(&np->lock);
    }

    // No child we could ever wait for (or we were killed).
    if(!havekids || p->killed){
      release(&wait_lock);
      return -1;
    }

    // Don't block: a matching child is alive, so report "no one exited".
    if(options & WNOHANG){
      release(&wait_lock);
      return 0;
    }

    sleep(p, &wait_lock);
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
#if PROC_SCHED_TRACE
      proc_sched_trace_dispatch(c, p);
#endif
      switchuvm(p);
      swtch(&c->context, &p->context);

      switchkvm();
#if PROC_SCHED_TRACE
      proc_sched_trace_save_outgoing(c, p);
#endif

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
      procvmtrace(p, "vmmap-manual");
      mmap_dump(p->vm);
      found++;
    }
    release(&p->lock);
  }
  printf("=== end occupied TTBR0 page-table entries ===\n\n");
  return found ? 0 : -1;
}
