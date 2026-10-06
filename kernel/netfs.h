#ifndef XV6_NETFS_H
#define XV6_NETFS_H

#define NETFS_MAGIC       0x4e465331U  // "NFS1"
#define NETFS_VERSION     1
#define NETFS_OP_STAT     1
#define NETFS_OP_READDIR  2
#define NETFS_OP_READ     3
#define NETFS_CLIENT_PORT 40500
#define NETFS_DEFAULT_PORT 5640
#define NETFS_DATA_MAX    1200

struct stat;

// All integer fields are in network byte order on the wire.  Requests append
// path_len bytes of an absolute path relative to the exported root.  Successful
// READ/READDIR replies append length bytes of data or a directory-entry name.
struct netfs_wire {
  uint32 magic;
  uint8 version;
  uint8 op;
  uint16 flags;
  uint32 xid;
  uint32 status;
  uint32 offset;
  uint32 length;
  uint32 ino;
  uint32 size;
  uint16 type;
  uint16 path_len;
} __attribute__((packed));

struct netfs_dirent {
  uint ino;
  uint size;
  short type;
  char name[15];
};

void netfsinit(void);
int  netfs_configure(char *source);
int  netfsstat(char *path, struct stat *st);
int  netfsread(char *path, uint64 off, void *dst, int n);
int  netfsreaddir(char *path, int index, struct netfs_dirent *de);

#endif
