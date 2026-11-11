#include <kernel/rwsem.h>

struct vm_area_struct;

// User FP/SIMD registers, saved and loaded by scheduler() (swtch.S).
struct fpsimd_state {
  uint64 vregs[64];            // q0..q31, low half first
  uint32 fpsr;
  uint32 fpcr;
  uint64 pad;
} __attribute__((aligned(16)));

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
// thread group (Linux mm_struct).  The lock serializes page-table changes and
// protects sz, refcount and changes to the mmap list; mmap_lock (sleepable)
// orders mmap/munmap against page faults, see kernel/mm/mmap.c.
struct vmspace {
  struct spinlock lock;
  int refcount;               // tasks and zombies holding the page table
  int users;                  // live tasks running on it (Linux mm_users)
  int execing;
  pagetable_t pagetable;
  uint64 sz;                  // high end of the low image/stack/heap region
  uint64 start_brk;           // first byte the process heap may use
  uint64 brk;                 // current heap break; pages are faulted lazily
  struct rw_semaphore mmap_lock;
  struct vm_area_struct *mmap; // mmap() regions, sorted by address
  int camera_mapped;          // number of /dev/video0 DMA mappings
};

// Reference-counted descriptor table (Linux files_struct).  CLONE_FILES
// tasks share one table; otherwise clone copies it.  lock protects count and
// fd[]; each non-zero fd[] entry owns one struct file reference.
struct files_struct {
  struct spinlock lock;
  int count;
  struct file *fd[NOFILE];
};

// Reference-counted filesystem context (Linux fs_struct).  CLONE_FS tasks
// share one, so chdir() in one is seen by all.  lock protects users, cwd and
// cwdpath; cwd is a native inode reference owned by the fs_struct.
struct fs_struct {
  struct spinlock lock;
  int users;
  struct inode *cwd;           // Current directory (native inode)
  char cwdpath[MAXPATH];       // Current directory as a normalized absolute
                               // path; relative paths are resolved against it
                               // so they also work below VFS mount points
};

// Files a system call looked up in a shared descriptor table.  Each holds a
// reference so a concurrent close() in a CLONE_FILES sibling cannot free the
// struct file mid-call; syscall() drops them when the handler returns.
#define NFPIN 4

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
  int vm_trace_exec_printed;   // first committed exec image was dumped
  int vm_trace_syscall_printed;// first syscall after that exec was dumped

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
  struct fpsimd_state fpsimd;  // user v0-v31/fpsr/fpcr while switched out
  void (*kthread_fn)(void*);   // kernel-thread entry, or zero for user process
  void *kthread_arg;
  struct files_struct *files;  // Descriptor table, shared by CLONE_FILES
  struct fs_struct *fs;        // cwd, shared by CLONE_FS
  struct file *fpin[NFPIN];    // References pinned by the current syscall
  int nfpin;

  // wait_lock must be held when using this:
  int vfork_pending;           // CLONE_VFORK parent waits until exec/exit
  char name[16];               // Process name (debugging)
};

extern struct proc *proc_head;
extern struct spinlock wait_lock;
