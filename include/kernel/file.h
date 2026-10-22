struct epoll;
struct pty;
struct file_operations;

#include <uapi/device.h>
#include <kernel/mutex.h>

struct file {
  enum { FD_NONE, FD_PIPE, FD_INODE, FD_DEVICE, FD_SOCKET,
         FD_EPOLL, FD_PTY } type;
  int ref; // reference count
  const struct file_operations *f_op; // operations for this open file description
  char readable;
  char writable;
  int flags;          // open-file-description status flags (O_NONBLOCK, ...)
  struct pipe *pipe; // FD_PIPE
  struct inode *ip;  // FD_INODE and FD_DEVICE
  struct mutex pos_lock; // serializes the shared open-file offset
  uint64 off;        // shared open-file offset; VFS backends may exceed 4 GiB
  short major;       // FD_DEVICE
  void *private_data; // FD_DEVICE: driver state from file_operations.open
  int socket;        // FD_SOCKET: TCP connection handle
  int socket_port;   // bound local TCP port before listen()
  int socket_backlog;
  struct epoll *epoll; // FD_EPOLL
  struct pty *pty;     // FD_PTY
  char pty_master;     // FD_PTY: master endpoint when non-zero
};

// in-memory copy of an inode
struct inode_ops;

#include <kernel/rwsem.h>

// In-memory inode of any mounted filesystem (see vfs.h).  dev names the
// filesystem instance (its super_block), iop its operations.
struct inode {
  uint dev;           // Device number = filesystem instance
  uint inum;          // Inode number within that filesystem
  int ref;            // Reference count
  struct inode_ops *iop; // operations of the filesystem (set by iget)
  char path[MAXPATH]; // path-based filesystems: path inside the filesystem
  struct rw_semaphore i_rwsem; // shared readers or one metadata writer
  int valid;          // inode has been read from disk?

  short type;         // copy of disk inode
  short major;
  short minor;
  short nlink;
  uint64 size;
  uint addrs[NDIRECT+1]; // xv6fs only
  uint32 fsdata[4];   // filesystem private (FAT32 write handle)
};
