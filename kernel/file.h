struct epoll;
struct pty;

struct file {
  enum { FD_NONE, FD_PIPE, FD_INODE, FD_DEVICE, FD_SOCKET,
         FD_EPOLL, FD_PTY } type;
  int ref; // reference count
  char readable;
  char writable;
  int flags;          // open-file-description status flags (O_NONBLOCK, ...)
  struct pipe *pipe; // FD_PIPE
  struct inode *ip;  // FD_INODE and FD_DEVICE
  uint64 off;        // shared open-file offset; VFS backends may exceed 4 GiB
  short major;       // FD_DEVICE
  short minor;       // FD_DEVICE instance within the major
  void *private_data; // FD_DEVICE: driver state from file_operations.open
  int socket;        // FD_SOCKET: TCP connection handle
  int socket_port;   // bound local TCP port before listen()
  int socket_backlog;
  struct epoll *epoll; // FD_EPOLL
  struct pty *pty;     // FD_PTY
  char pty_master;     // FD_PTY: master endpoint when non-zero
};

#define major(dev)  ((dev) >> 16 & 0xFFFF)
#define minor(dev)  ((dev) & 0xFFFF)
#define	mkdev(m,n)  ((uint)((m)<<16| (n)))

// in-memory copy of an inode
struct inode_ops;

// In-memory inode of any mounted filesystem (see vfs.h).  dev names the
// filesystem instance (its super_block), iop its operations.
struct inode {
  uint dev;           // Device number = filesystem instance
  uint inum;          // Inode number within that filesystem
  int ref;            // Reference count
  struct inode_ops *iop; // operations of the filesystem (set by iget)
  char path[MAXPATH]; // path-based filesystems: path inside the filesystem
  struct sleeplock lock; // protects everything below here
  int valid;          // inode has been read from disk?

  short type;         // copy of disk inode
  short major;
  short minor;
  short nlink;
  uint64 size;
  uint addrs[NDIRECT+1]; // xv6fs only
  uint32 fsdata[4];   // filesystem private (FAT32 write handle)
};

#define CONSOLE 1
#define TTY     2   // current process controlling terminal (/dev/tty)
#define TTYS0   3   // Mini UART terminal (/dev/ttyS0)
#define INPUT   4   // input subsystem evdev (/dev/input/event0)
#define CAMERA  5   // Unicam CSI-2 camera (/dev/video0)
#define ROOTUPDATE 6 // guarded raw-root updater (/dev/sdroot)
#define BLOCKDEV 7  // block device files, minor = blkdev number (/dev/mmcblk0p2)
