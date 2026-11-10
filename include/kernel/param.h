// Process descriptors and kernel stacks are allocated on demand; there is no
// fixed NPROC table.  NPROC_MAX caps how many are ever created, like Linux's
// threads-max (default: RAM pages / 32, i.e. 896 for 112 MiB).  Before
// copy-on-write fork, memory ran out first; now a child costs only a few
// pages, and without a cap one runaway fork loop pins all of RAM in slab
// caches that are never shrunk.
#include <uapi/limits.h>

#define NPROC_MAX   896  // maximum number of tasks (threads-max)
#define NOFILE       16  // open files per process
#define NFILE       100  // filesystem-side open/orphan tracking slots
#define NDEV         10  // maximum major device number
#define USTACKPAGES   4  // mapped user stack pages, excluding guard page
#define LOGSIZE      (MAXOPBLOCKS*3)  // max data blocks in on-disk log
#define NBUF         64   // buffer cache entries, one 4 KiB page each
#define NAMEMAX      128   // maximum length of one path component (VFS)
#define NSYNC         64   // maximum number of kernel synchronization objects
#define NUDPPORT      16   // maximum number of bound UDP ports
#define NUDPQUEUE     16   // datagrams queued per UDP port
