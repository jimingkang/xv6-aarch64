// Process descriptors and kernel stacks are allocated on demand; there is no
// fixed NPROC table.  Available physical memory is the practical limit.
#ifdef XV6_ARM32
#define NCPU 1  // single-core T113 bringup
#else
#define NCPU 4  // maximum number of CPUs
#endif
#define NOFILE       16  // open files per process
#define NFILE       100  // open files per system
#define NINODE       50  // maximum number of active i-nodes
#define NDEV         10  // maximum major device number
#define ROOTDEV       1  // device number of file system root disk
#define MAXARG       32  // max exec arguments
#define USTACKPAGES   4  // mapped user stack pages, excluding guard page
#define MAXOPBLOCKS  10  // max # of blocks any FS op writes
#define LOGSIZE      (MAXOPBLOCKS*3)  // max data blocks in on-disk log
#define NBUF         (MAXOPBLOCKS*3)  // size of disk block cache
#ifdef FSSIZE_T113
#define FSSIZE FSSIZE_T113
#else
#define FSSIZE      32768  // 32 MiB native file system (1024-byte blocks)
#endif
#define MAXPATH      128   // maximum file path name
#define NSYNC         64   // maximum number of kernel synchronization objects
#define NUDPPORT      16   // maximum number of bound UDP ports
#define NUDPQUEUE     16   // datagrams queued per UDP port
