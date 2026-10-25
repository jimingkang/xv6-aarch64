#ifndef XV6_VFS_H
#define XV6_VFS_H

// The virtual filesystem switch (compare Linux struct super_block and
// struct inode_operations).
//
// Every mounted filesystem instance has a super_block, indexed by its device
// number: a block device for disk filesystems (xv6fs on dev 1, ext2 on
// mmcblk0p3 = dev 5), a pseudo number for procfs, FAT32 and netfs.  Every
// in-memory struct inode carries the operations of its filesystem, so path
// lookup (namex), open/read/write, mkdir, unlink and rename work the same
// way on all of them, and one mount table (fs.c) joins them into one tree.

struct inode;
struct stat;
struct super_block;

#define VFS_MOUNT_RDONLY  1
#define VFS_MOUNT_ROOTLOG 0x100   // kernel only: xv6fs attaches its log (root)

#define NSUPER     24          // device numbers 0..NSUPER-1
#define PROCFS_DEV 20
#define FATFS_DEV  21
#define NETFS_DEV  22
#define SYSFS_DEV  23

// Locking conventions, as in xv6:
//   read_inode  called by ilock() with ip->lock held, fills type/nlink/size
//   evict       called by iput() for the last reference, ip->lock held
//   lookup      dp locked; returns a referenced, unlocked inode or 0
//   create      dp referenced and unlocked, reference consumed; returns the
//               new inode locked (for T_FILE also an existing file), or 0
//   unlink/link/rename  directory arguments referenced and unlocked
//   truncate  ip locked
// File data I/O and per-open hooks live in inode.i_fop/file.f_op, not here.
// Directories of every filesystem read as a stream of xv6 struct dirent.
struct inode_ops {
  int  (*read_inode)(struct inode *ip);
  void (*evict)(struct inode *ip);
  struct inode *(*lookup)(struct inode *dp, char *name);
  struct inode *(*create)(struct inode *dp, char *name, short type,
                          short major, short minor);
  int  (*unlink)(struct inode *dp, char *name);
  int  (*link)(struct inode *dp, char *name, struct inode *ip);
  int  (*rename)(struct inode *odp, char *oname, struct inode *ndp,
                 char *nname);
  int  (*truncate)(struct inode *ip, uint64 size);
  int  (*readlink)(struct inode *ip, char *buf, int bufsz);
};

struct super_block {
  int used;
  int dev;
  char type[12];             // "xv6fs", "ext2", "fat32", "procfs", "netfs"
  struct inode_ops *iop;
  uint rootino;              // inode number of the root directory
  int readonly;
  int logged;                // xv6fs: writes go through the log in chunks
  int pathbased;             // inodes named by path (FAT32, procfs, netfs)
  void *priv;                // filesystem private data
  // Called after the mount is detached from pathname lookup and all inode
  // references have drained.  A disk filesystem uses this to write its clean
  // state, flush/invalidate caches and release per-mount memory.
  int (*put_super)(struct super_block *sb);
};

struct super_block *getsuper(uint dev);
int  fs_super_register(int dev, char *type, struct inode_ops *iop,
                       uint rootino, int readonly, int logged,
                       int pathbased, void *priv);
int  fs_super_set_put_super(int dev, int (*put_super)(struct super_block*));
int  fs_umount(char *target);
// Directory stream helper for filesystems without xv6 on-disk dirents:
// readdir(dp, index, name, &ino) returns 1 for an entry, 0 at the end.
int  vfs_dir_read(struct inode *dp, int user_dst, uint64 dst, uint64 off,
                  uint n, int (*readdir)(struct inode*, int, char*, uint*));

int  vfsmount(char *source, char *target, char *fstype, int flags);
int  vfsumount(char *target);

// A filesystem type (compare Linux struct file_system_type).  Each type is
// registered once at boot; mount(2) and mount_root() find it by name.
//   get_sb      mount source -> device number whose supers[dev] is installed
//               (calls fill_super the first time that dev is mounted)
//   fill_super  read one instance's superblock and fs_super_register() it;
//               0 for types that need no per-device work
struct fs_type {
  char *name;
  int  (*get_sb)(struct fs_type *t, char *source, int flags);
  int  (*fill_super)(int dev, int flags);
  struct fs_type *next;
};

int  register_filesystem(struct fs_type *t);
struct fs_type *get_fs_type(char *name);
int  get_sb_bdev(struct fs_type *t, char *source, int flags);
int  vfs_get_sb(char *fstype, char *source, int flags);
int  lookup_bdev(char *path);

#endif
