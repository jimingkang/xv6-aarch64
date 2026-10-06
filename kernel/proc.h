// Saved registers for kernel context switches.
struct context {
  uint64 sp;

  /* callee register */
  uint64 x18;
  uint64 x19;
  uint64 x20;
  uint64 x21;
  uint64 x22;
  uint64 x23;
  uint64 x24;
  uint64 x25;
  uint64 x26;
  uint64 x27;
  uint64 x28;
  uint64 x29;
  uint64 x30;
};

// Per-CPU state.
struct cpu {
  struct proc *proc;          // The process running on this cpu, or null.
  struct context context;     // swtch() here to enter scheduler().
  int noff;                   // Depth of push_off() nesting.
  int intena;                 // Were interrupts enabled before push_off()?

  // Scheduler state.  Written by this CPU, read locklessly by others as a
  // hint when they decide whether to preempt it.
  volatile int online;        // has entered scheduler()
  volatile int need_resched;  // yield at the next interrupt/syscall return
  volatile int cur_prio;      // effective priority of c->proc, PRIO_IDLE if none
  int rt_window;              // ticks in the current RT throttling period
  int rt_used;                // ticks spent running RT tasks in that period
  volatile int rt_throttled;  // RT budget exhausted: run SCHED_OTHER first
  uint rt_throttle_events;
};

extern struct cpu cpus[NCPU];

// per-process data for the trap handling code in trampoline.S.
// sits in a page by itself just under the trampoline page in the
// user page table. not specially mapped in the kernel page table.
// the sscratch register points here.
// uservec in trampoline.S saves user registers in the trapframe,
// then initializes registers from the trapframe's
// kernel_sp, kernel_hartid, kernel_satp, and jumps to kernel_trap.
// usertrapret() and userret in trampoline.S set up
// the trapframe's kernel_*, restore user registers from the
// trapframe, switch to the user page table, and enter user space.
// the trapframe includes callee-saved user registers like s0-s11 because the
// return-to-user path via usertrapret() doesn't return through
// the entire kernel call stack.
struct trapframe {
  uint64 x0;
  uint64 x1;
  uint64 x2;
  uint64 x3;
  uint64 x4;
  uint64 x5;
  uint64 x6;
  uint64 x7;
  uint64 x8;
  uint64 x9;
  uint64 x10;
  uint64 x11;
  uint64 x12;
  uint64 x13;
  uint64 x14;
  uint64 x15;
  uint64 x16;
  uint64 x17;
  uint64 x18;
  uint64 x19;
  uint64 x20;
  uint64 x21;
  uint64 x22;
  uint64 x23;
  uint64 x24;
  uint64 x25;
  uint64 x26;
  uint64 x27;
  uint64 x28;
  uint64 x29;
  uint64 x30;
  uint64 elr;
  uint64 spsr;
  uint64 sp;     
};


enum procstate { UNUSED, USED, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

#define SIGINT 2

// Reference-counted user address space shared by all user threads in a
// thread group.  The lock serializes page-table changes and protects sz and
// refcount.
struct vmspace {
  struct spinlock lock;
  int refcount;
  int execing;
  pagetable_t pagetable;
  uint64 sz;
};

// Per-process state
struct proc {
  struct spinlock lock;

  // p->lock must be held when using these:
  enum procstate state;        // Process state
  void *chan;                  // If non-zero, sleeping on chan
  int killed;                  // If non-zero, have been killed
  int xstate;                  // Exit status to be returned to parent's wait
  int pid;                     // Thread ID (also PID for a process leader)
  int tgid;                    // Thread-group/process ID
  int is_thread;               // Shares vmspace with another proc slot
  int sid;                     // Session ID
  int pgid;                    // Process group ID
  int ctty;                    // Controlling TTY major, or -1
  uint signals_pending;       // Pending kernel-generated signals

  // Scheduling attributes (see sched.h), protected by p->lock.
  int policy;                  // SCHED_OTHER, SCHED_FIFO or SCHED_RR
  int rt_priority;             // 1..99 for real-time policies, else 0
  int nice;                    // -20..19, SCHED_OTHER time-slice weight
  uint cpumask;                // CPUs this process may run on
  int slice;                   // scheduler ticks left in the current slice
  uint64 rq_seq;               // FIFO order among equal priorities
  int last_cpu;                // CPU that ran it last, -1 if never
  uint64 run_ticks;            // scheduler ticks consumed

  // wait_lock must be held when using this:
  struct proc *parent;         // Parent process

  // these are private to the process, so p->lock need not be held.
  uint64 kstack;               // Virtual address of kernel stack
  uint64 kstack_pa;            // Physical backing page, allocated on demand
  uint64 kstack_slot;          // Stable dynamic KSTACK() virtual slot
  struct proc *next;           // Immutable growable process-table link
  struct vmspace *vm;          // Shared user address space
  struct trapframe *trapframe; // data page for trampoline.S
  struct context context;      // swtch() here to run process
  void (*kthread_fn)(void*);   // kernel-thread entry, or zero for user process
  void *kthread_arg;
  struct file *ofile[NOFILE];  // Open files
  struct inode *cwd;           // Current directory (native inode)
  char cwdpath[MAXPATH];       // Current directory as a normalized absolute
                               // path; relative paths are resolved against it
                               // so they also work below VFS mount points
  char name[16];               // Process name (debugging)
};

extern struct proc *proc_head;
