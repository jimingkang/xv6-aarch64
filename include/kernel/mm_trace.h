#ifndef XV6_MM_TRACE_H
#define XV6_MM_TRACE_H

// MM_TRACE: memory-management teaching trace.  `make MM_TRACE=1` prints one
// line per event on every layer of the allocator stack:
//
//   page   alloc_pages/kalloc, __free_pages/kfree, put_page (order, pa,
//          per-CPU list or buddy, caller), pcp refill / give-back / drain
//   slab   kmem_cache_alloc/free, new slab page, empty slab returned,
//          kmalloc size class or large allocation
//   walk   page-table pages allocated by walk(), and the L1/L2/L3
//          descriptors of every user page fault after it is resolved
//   fault  user page faults: address, read/write, how they were resolved
//          (copy-on-write copy/reuse, brk zero page, VMA fault)
//   brk / mmap / munmap system calls with their arguments and results
//
// Each line is prefixed "mm[pid name]" and ends with the caller's return
// address where useful; look it up in kernel/kernel.sym or with
// aarch64-linux-gnu-addr2line -e kernel/kernel.
//
//   MM_TRACE_PID  -1 (default) user processes only, 0 everything including
//                 kernel threads and interrupts, N only pid N
//   MM_TRACE_LIMIT  stop after this many lines (0 = unlimited)
//
// Off by default: these are the hottest paths in the kernel, and serial
// output slows them by orders of magnitude.
#ifndef MM_TRACE
#define MM_TRACE 0
#endif
#ifndef MM_TRACE_PID
#define MM_TRACE_PID -1
#endif
#ifndef MM_TRACE_LIMIT
#define MM_TRACE_LIMIT 0
#endif

int   mm_trace_on(void);
int   mm_trace_pid(void);
char *mm_trace_comm(void);
extern int mm_trace_enabled;    // set by main() once printf is usable

#if MM_TRACE
#define MMTRACE(fmt, ...)                                                   \
  do {                                                                      \
    if(mm_trace_on())                                                       \
      printf("mm[%d %s] " fmt, mm_trace_pid(), mm_trace_comm(), ##__VA_ARGS__); \
  } while(0)
#else
// Type-check the arguments, then let the optimizer drop everything.
#define MMTRACE(fmt, ...)                                                   \
  do { if(0) printf("mm " fmt, ##__VA_ARGS__); } while(0)
#endif

#define MM_CALLER() __builtin_return_address(0)

#endif
