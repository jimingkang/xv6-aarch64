struct vnode;
struct epoll;
struct pty;

struct file {
  enum { FD_NONE, FD_PIPE, FD_INODE, FD_DEVICE, FD_VNODE, FD_SOCKET,
         FD_EPOLL, FD_PTY } type;
  int ref; // reference count
  char readable;
  char writable;
  int flags;          // open-file-description status flags (O_NONBLOCK, ...)
  struct pipe *pipe; // FD_PIPE
  struct inode *ip;  // FD_INODE and FD_DEVICE
  struct vnode *vn;  // FD_VNODE
  uint off;          // FD_INODE
  short major;       // FD_DEVICE
  void *private_data; // FD_DEVICE: driver state from file_operations.open
  int socket;        // FD_SOCKET: TCP connection handle
  struct epoll *epoll; // FD_EPOLL
  struct pty *pty;     // FD_PTY
  char pty_master;     // FD_PTY: master endpoint when non-zero
};

#define major(dev)  ((dev) >> 16 & 0xFFFF)
#define minor(dev)  ((dev) & 0xFFFF)
#define	mkdev(m,n)  ((uint)((m)<<16| (n)))

// in-memory copy of an inode
struct inode {
  uint dev;           // Device number
  uint inum;          // Inode number
  int ref;            // Reference count
  struct sleeplock lock; // protects everything below here
  int valid;          // inode has been read from disk?

  short type;         // copy of disk inode
  short major;
  short minor;
  short nlink;
  uint size;
  uint addrs[NDIRECT+1];
};

#define CONSOLE 1
#define TTY     2   // current process controlling terminal (/dev/tty)
#define TTYS0   3   // Mini UART terminal (/dev/ttyS0)
#define INPUT   4   // input subsystem evdev (/dev/input/event0)
