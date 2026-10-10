// Mounting the root filesystem, the embedded-Linux way
// (compare Linux init/do_mounts.c, init/noinitramfs.c, drivers/base/devtmpfs.c).
//
// Linux without an initramfs boots like this:
//
//   init_mount_tree()     "/" is rootfs, a filesystem in RAM
//   default_rootfs()      mkdir /dev, mknod /dev/console, mkdir /root
//   devtmpfs_init()       a private RAM filesystem the driver core fills
//   prepare_namespace()   ROOT_DEV = name_to_dev_t(root=)   (rootdev.c)
//     mount_root()        mknod /dev/root, mount it on /root, chdir /root
//     devtmpfs_mount()    mount devtmpfs on dev (= /root/dev)
//     mount(".", "/", MS_MOVE)   the real root now covers rootfs
//     chroot(".")         and becomes "/" for init
//   run_init_process("/init")
//
// xv6 does the same steps in forkret() of the first process, because the
// buffer cache and inode locks need a process context.  Linux's rootfs and
// devtmpfs are ramfs/tmpfs; xv6's inode layer sits on the buffer cache, so
// here they are ordinary xv6 filesystems on two RAM disks (blkdev.c),
// formatted in memory at boot and gone at reboot.

#include "types.h"
#include "aarch64.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "fs.h"
#include "buf.h"
#include "file.h"
#include "stat.h"
#include "defs.h"
#include "blkdev.h"
#include "workqueue.h"
#include "device.h"
#include "input.h"

#define ROOTFS_DEV    BLKDEV_RAM(0)
#define DEVTMPFS_DEV  BLKDEV_RAM(1)

// Format an empty xv6 filesystem directly in RAM-disk memory, like mkfs:
// [ boot | super | inodes | bitmap | data ].  No log: RAM-disk blocks are
// written through (log_write()).
static void
ramfs_mkfs(int dev, uint nblocks, uint ninodes)
{
  struct superblock sb;
  uint ninodeblocks = ninodes / IPB + 1;
  uint nbitmap = nblocks / BPB + 1;
  uint datastart = 2 + ninodeblocks + nbitmap;

  memset(&sb, 0, sizeof(sb));
  sb.magic = FSMAGIC;
  sb.size = nblocks;
  sb.nblocks = nblocks - datastart;
  sb.ninodes = ninodes;
  sb.nlog = 0;
  sb.logstart = 2;
  sb.inodestart = 2;
  sb.bmapstart = 2 + ninodeblocks;
  memmove(ramdisk_ptr(dev, BSIZE), &sb, sizeof(sb));

  // Root directory: inode 1 with "." and ".." in the first data block.
  struct dinode *di = (struct dinode *)ramdisk_ptr(dev,
                        IBLOCK(ROOTINO, sb) * BSIZE) + ROOTINO % IPB;
  di->type = T_DIR;
  di->nlink = 1;
  di->size = 2 * sizeof(struct dirent);
  di->addrs[0] = datastart;
  struct dirent *de = (struct dirent *)ramdisk_ptr(dev, datastart * BSIZE);
  de[0].inum = ROOTINO;
  safestrcpy(de[0].name, ".", DIRSIZ);
  de[1].inum = ROOTINO;
  safestrcpy(de[1].name, "..", DIRSIZ);

  // Metadata blocks and the root directory block are in use.
  uchar *bm = ramdisk_ptr(dev, sb.bmapstart * BSIZE);
  for(uint b = 0; b <= datastart; b++)
    bm[b / 8] |= 1 << (b % 8);
}

static void
ramfs_create(int n, int dev, char *name, uint nblocks, uint ninodes)
{
  if(ramdisk_create(n, dev, name, nblocks * BSIZE, BSIZE) < 0)
    panic("ramfs: no RAM disk");
  ramfs_mkfs(dev, nblocks, ninodes);
  if(fs_readsuper(dev) < 0)
    panic("ramfs: bad superblock");
}

// Kernel-internal file system calls (Linux: init_mkdir(), init_mknod() ...).

static int
kmknod(char *path, short type, short major, short minor)
{
  struct inode *dp, *ip;
  char name[NAMEMAX];

  begin_op();
  if((dp = nameiparent(path, name)) == 0){
    end_op();
    return -1;
  }
  ip = fs_create(dp, name, type, major, minor);
  if(ip)
    iunlockput(ip);
  end_op();
  return ip ? 0 : -1;
}

static struct inode*
klookup(char *path)
{
  begin_op();
  struct inode *ip = namei(path);
  end_op();
  return ip;
}

static void
kput(struct inode *ip)
{
  begin_op();
  iput(ip);
  end_op();
}

static int
kchdir(char *path)
{
  struct proc *p = myproc();
  struct inode *ip = klookup(path);
  if(ip == 0)
    return -1;
  ilock(ip);
  int dir = ip->type == T_DIR;
  iunlock(ip);
  if(!dir){
    kput(ip);
    return -1;
  }
  if(p->cwd)
    kput(p->cwd);
  p->cwd = ip;
  return 0;
}

// ---------------------------------------------------------------------------
// devtmpfs: /dev maintained by the kernel.  Every registered character
// device (register_chrdev() name, e.g. "input/event0") and every block
// device of the SD card gets a node; drivers registering later are added by
// a work item.  User space no longer has to mknod anything.

static int devtmpfs_ready;
static struct work_struct devtmpfs_work;

// Create "a/b/name" below the devtmpfs root, making directories as needed.
// Existing nodes are left alone.  Returns 1 if a node was created.
static int
devtmpfs_create_node(char *path, short major, short minor)
{
  char elem[DIRSIZ];
  int created = 0;

  begin_op();
  struct inode *dp = fs_dev_root(DEVTMPFS_DEV), *ip;
  while(*path){
    char *e = path;
    int len;
    while(*e && *e != '/')
      e++;
    len = e - path;
    if(len >= DIRSIZ)
      len = DIRSIZ - 1;
    memmove(elem, path, len);
    elem[len] = 0;
    path = *e ? e + 1 : e;

    ilock(dp);
    ip = dirlookup(dp, elem, 0);
    iunlock(dp);
    if(ip){                           // exists: descend or stop
      iput(dp);
      dp = ip;
      continue;
    }
    ip = fs_create(dp, elem, *path ? T_DIR : T_DEVICE,
                   *path ? 0 : major, *path ? 0 : minor);   // consumes dp
    if(ip == 0){
      end_op();
      return created;
    }
    iunlock(ip);
    created = 1;
    dp = ip;
  }
  iput(dp);
  end_op();
  return created;
}

static int
devtmpfs_populate(void)
{
  int n = 0;
  for(int major = 0; major < NDEV; major++){
    char *name = chrdev_name(major);
    if(name && major != BLOCKDEV){
      n += devtmpfs_create_node(name, major, 0);
      if(major == INPUT){
        static char *events[] = {
          "input/event1", "input/event2", "input/event3"
        };
        for(int minor = 1; minor < INPUT_MAX_DEVICES; minor++)
          n += devtmpfs_create_node(events[minor - 1], major, minor);
      }
    }
  }
  for(int i = 0; i < NBLKDEV; i++){
    struct blkdev *bd = blkdev_at(i);
    if(bd && bd->dev != ROOTDEV)        // /dev/root lives only in rootfs
      n += devtmpfs_create_node(bd->name, BLOCKDEV, bd->dev);
  }
  return n;
}

static void
devtmpfs_worker(struct work_struct *w)
{
  int n = devtmpfs_populate();
  if(n)
    printf("devtmpfs: %d new node%s\n", n, n == 1 ? "" : "s");
}

// register_chrdev() hook.  Before devtmpfs exists the initial scan will
// find the device anyway.
void
devtmpfs_notify(int major)
{
  if(devtmpfs_ready)
    schedule_work(&devtmpfs_work);
}

static void
devtmpfs_init(void)
{
  ramfs_create(1, DEVTMPFS_DEV, "ram1", 64, 48);
  int n = devtmpfs_populate();
  init_work(&devtmpfs_work, devtmpfs_worker);
  devtmpfs_ready = 1;
  printf("devtmpfs: initialized on ram1, %d nodes\n", n);
}

// Mount devtmpfs on "dev" relative to the current directory (Linux:
// devtmpfs_mount(), called with the cwd at the future root).
static void
devtmpfs_mount(char *path, char *shown)
{
  struct inode *mp = klookup(path);
  if(mp == 0 || fs_mount(mp, DEVTMPFS_DEV, "devtmpfs", "devtmpfs",
                         shown) < 0){
    printf("devtmpfs: error mounting on %s\n", shown);
    if(mp)
      kput(mp);
    return;
  }
  printf("devtmpfs: mounted\n");
}

// ---------------------------------------------------------------------------

// The block device a device node names: /dev/root -> its minor number.
static int
node_to_blkdev(char *devname)
{
  struct inode *ip = klookup(devname);
  int dev = -1;

  if(ip){
    ilock(ip);
    if(ip->type == T_DEVICE && ip->major == BLOCKDEV)
      dev = ip->minor;
    iunlock(ip);
    kput(ip);
  }
  return dev;
}

// mount_root(): mount the filesystem on the block device named by the node
// devname on directory dir.  rootfstype= picks the filesystem: xv6fs is read
// by fsinit() (superblock + log recovery), ext2 was opened by ext2init() and
// is checked against the node.  Both become an ordinary entry of the one
// mount table, so both are MS_MOVEd and chrooted the same way afterwards.
static void
mount_root(char *devname, char *dir)
{
  int dev = node_to_blkdev(devname);
  char *type;
  struct blkdev *bd = dev > 0 ? blkdev_get(dev) : 0;

  if(rootdev_fstype() == ROOTFS_EXT2){
    if(bd == 0 || !ext2ready() || bd->start != ext2_part_lba() ||
       ext2_register_super(dev, 0) < 0)
      panic("VFS: Unable to mount root fs (ext2) on /dev/root");
    type = "ext2";
  } else {
    // fill_super: superblock check and log recovery.
    if(dev < 0 || fsinit(dev) < 0)
      panic("VFS: Unable to mount root fs on /dev/root");
    type = "xv6fs";
  }
  struct inode *mp = klookup(dir);
  if(mp == 0 || fs_mount(mp, dev, type, devname, dir) < 0)
    panic("VFS: cannot attach root");
  printf("VFS: Mounted root (%s filesystem) on device %d (/dev/root = %s).\n",
         type, dev, rootdev_fstype() == ROOTFS_EXT2 ? bd->name : rootdev_name());
}

void
prepare_namespace(void)
{
  struct proc *p = myproc();

  // rootfs: the first "/".
  ramfs_create(0, ROOTFS_DEV, "ram0", 32, 16);
  struct inode *r = fs_dev_root(ROOTFS_DEV);
  fs_set_root(r);
  p->cwd = r;
  kmknod("/dev", T_DIR, 0, 0);
  kmknod("/dev/console", T_DEVICE, CONSOLE, 0);
  kmknod("/root", T_DIR, 0, 0);
  printf("rootfs: ram0 is /, with /dev/console and /root\n");

  devtmpfs_init();

  // mount_root(): ROOT_DEV, chosen by rootdev_init() from root=, is dev 1
  // for an xv6fs root and the ext2 partition (mmcblk0pN) for an ext2 root.
  int rootdev = rootdev_fstype() == ROOTFS_EXT2 ?
                BLKDEV_MMC_PART(rootdev_ext2_part()) : ROOTDEV;
  kmknod("/dev/root", T_DEVICE, BLOCKDEV, rootdev);
  mount_root("/dev/root", "/root");
  if(kchdir("/root") < 0)
    panic("prepare_namespace: chdir /root");

  // A freshly made fs.img has no /dev yet (init used to create it); make
  // the mount point instead of booting without devtmpfs.
  struct inode *d = klookup("dev");
  if(d)
    kput(d);
  else if(kmknod("dev", T_DIR, 0, 0) == 0)
    printf("devtmpfs: created /dev on the root filesystem\n");
  devtmpfs_mount("dev", "/root/dev");

  // mount(".", "/", MS_MOVE): the root mount leaves /root and covers "/".
  struct inode *from = idup(p->cwd), *to = klookup("/");
  if(fs_move_mount(from, to, "/") < 0)
    panic("prepare_namespace: MS_MOVE");
  kput(from);

  // chroot("."): "/" is now the disk root for every process.
  fs_set_root(p->cwd);
  safestrcpy(p->cwdpath, "/", sizeof(p->cwdpath));
  printf("VFS: root moved over rootfs, chroot done; running /init\n");
}
