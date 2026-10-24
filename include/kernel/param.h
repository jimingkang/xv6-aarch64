// Process descriptors and kernel stacks are allocated on demand; there is no
// fixed NPROC table.  Available physical memory is the practical limit.
#include <uapi/limits.h>

#define NOFILE       16  // open files per process
#define NFILE       100  // open files per system
#define NDEV         10  // maximum major device number
#define USTACKPAGES   4  // mapped user stack pages, excluding guard page
#define LOGSIZE      (MAXOPBLOCKS*3)  // max data blocks in on-disk log
#define NBUF         64   // buffer cache entries, one 4 KiB page each
#define NAMEMAX      128   // maximum length of one path component (VFS)
#define NSYNC         64   // maximum number of kernel synchronization objects
#define NUDPPORT      16   // maximum number of bound UDP ports
#define NUDPQUEUE     16   // datagrams queued per UDP port
