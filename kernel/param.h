#define NPROC        64  // maximum number of processes
#define NCPU          4  // maximum number of CPUs
#define NOFILE       16  // open files per process
#define NFILE       100  // open files per system
#define NINODE       50  // maximum number of active i-nodes
#define NDEV         10  // maximum major device number
#define ROOTDEV       1  // device number of file system root disk
#define MAXARG       32  // max exec arguments
#define MAXOPBLOCKS  10  // max # of blocks any FS op writes
#define LOGSIZE      (MAXOPBLOCKS*3)  // max data blocks in on-disk log
#define NBUF         (MAXOPBLOCKS*3)  // size of disk block cache
#define FSSIZE      32768  // 32 MiB native file system (1024-byte blocks)
#define MAXPATH      128   // maximum file path name
#define NSYNC         64   // maximum number of kernel synchronization objects
#define NUDPPORT      16   // maximum number of bound UDP ports
#define NUDPQUEUE     16   // datagrams queued per UDP port
