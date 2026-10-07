// Small vnode layer. The native xv6 inode filesystem remains the writable
// root; init mounts backends named in /etc/fstab through mount(2).

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "aarch64.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "fcntl.h"
#include "stat.h"
#include "ext2.h"
#include "fat32.h"
#include "netfs.h"
#include "defs.h"
#include "vfs.h"
#include "device.h"

extern char etext[];
extern char end[];

struct vnode_ops {
  int (*stat)(char*, struct stat*);
  int (*read)(char*, uint64, void*, int);
  int (*write)(char*, struct fat32_file*, uint64, int, uint64, int);
  int (*readdir)(char*, int, void*);
  int (*create)(char*);
  int (*truncate)(char*, uint64);
  int (*rename)(char*, char*);
  int (*unlink)(char*);
  int (*mkdir)(char*);
  int (*fsync)(void);
  // Optional inode-bound file I/O.  A backend that provides open() returns a
  // handle naming the object itself; later I/O on the vnode uses the handle,
  // so unlink/rename of the path cannot redirect an open descriptor.
  int (*open)(char*, uint64*);
  void (*release)(uint64);
  int (*hstat)(uint64, struct stat*);
  int (*hread)(uint64, uint64, void*, int);
  int (*hwrite)(uint64, uint64, int, uint64, int);
  int (*htruncate)(uint64, uint64);
};

struct vfs_dirent {
  uint ino;
  uint64 size;
  short type;
  char name[EXT2_NAME_MAX + 1];
};

struct vnode {
  int used;
  int ref;
  short type;
  char path[MAXPATH];
  struct vnode_ops *ops;
  int readonly;
  int has_handle;
  uint64 handle;
  struct fat32_file fat_file;
};

static struct {
  struct spinlock lock;
  struct vnode nodes[NFILE];
} vnodes;

#define NMOUNT 8
struct vfs_mount {
  int used;
  char path[MAXPATH];
  char source[24];          // for /proc/mounts
  struct vnode_ops *ops;
  int readonly;
};
static struct vfs_mount mounts[NMOUNT];

static int
ext2_vstat(char *path, struct stat *st)
{
  uint ino;
  uint64 size;
  ushort mode;
  if(ext2stat(path, &ino, &mode, &size) < 0)
    return -1;
  memset(st, 0, sizeof(*st));
  st->dev = 2;
  st->ino = ino;
  st->type = (mode & 0xf000) == 0x4000 ? T_DIR : T_FILE;
  st->nlink = 1;
  st->size = size;
  return 0;
}

static int
ext2_vhstat(uint64 handle, struct stat *st)
{
  uint ino;
  uint64 size;
  ushort mode;
  if(ext2statino(handle, &ino, &mode, &size) < 0)
    return -1;
  memset(st, 0, sizeof(*st));
  st->dev = 2;
  st->ino = ino;
  st->type = (mode & 0xf000) == 0x4000 ? T_DIR : T_FILE;
  st->nlink = 1;
  st->size = size;
  return 0;
}

static int
ext2_vreaddir(char *path, int index, void *arg)
{
  struct ext2_user_dirent ede;
  struct vfs_dirent *de = arg;
  int r = ext2readdir(path, index, &ede);
  if(r <= 0)
    return r;
  memset(de, 0, sizeof(*de));
  de->ino = ede.inode;
  de->size = ede.size;
  de->type = (ede.mode & 0xf000) == 0x4000 ? T_DIR : T_FILE;
  safestrcpy(de->name, ede.name, sizeof(de->name));
  return 1;
}

static struct vnode_ops ext2_ops = {
  .stat = ext2_vstat,
  .read = ext2readfile,
  .write = ext2writefile,
  .readdir = ext2_vreaddir,
  .create = ext2createfile,
  .truncate = ext2truncatefile,
  .rename = ext2rename,
  .unlink = ext2unlink,
  .mkdir = ext2mkdir,
  .fsync = ext2fsync,
  .open = ext2open,
  .release = ext2release,
  .hstat = ext2_vhstat,
  .hread = ext2readino,
  .hwrite = ext2writeino,
  .htruncate = ext2truncino,
};

static int
fat_vstat(char *path, struct stat *st)
{
  struct fat32_dirent fe;
  if(fat32statpath(path, &fe) < 0) return -1;
  memset(st, 0, sizeof(*st));
  st->dev = 4; st->ino = fe.cluster ? fe.cluster : 1;
  st->type = fe.directory ? T_DIR : T_FILE;
  st->nlink = 1; st->size = fe.size;
  return 0;
}

static int
fat_vread(char *path, uint64 off, void *dst, int n)
{
  struct fat32_file file;
  struct fat32_dirent fe;
  if(fat32statpath(path, &fe) < 0 || fe.directory) return -1;
  if(fat32openpath(path, &file) < 0) return -1;
  return fat32pread(&file, off, dst, n);
}

static int
fat_vwrite(char *path, struct fat32_file *file, uint64 off, int user_src,
           uint64 src, int n)
{
  return fat32writefile(path, file, off, user_src, src, n);
}

static int
fat_vcreate(char *path)
{
  return fat32createfile(path);
}

static int
fat_vtruncate(char *path, uint64 size)
{
  struct fat32_file file;
  if(size != 0 || fat32openwrite(path, &file) < 0)
    return -1;
  return fat32truncatefile(path, &file);
}

static int
fat_vfsync(void)
{
  return sdflush();
}

static int
fat_vrename(char *oldpath, char *newpath)
{
  return fat32renamefile(oldpath, newpath);
}

static int
fat_vreaddir(char *path, int index, void *arg)
{
  struct fat32_dirent fe;
  struct vfs_dirent *de = arg;
  int r;
  if(path[0] != '/' || path[1] != 0) return 0;
  r = fat32readdirroot(index, &fe);
  if(r <= 0) return r;
  memset(de, 0, sizeof(*de));
  // Empty FAT files legitimately have first-cluster zero.  xv6 ls treats
  // inode zero as an unused directory slot, so synthesize a stable non-zero
  // number for those entries.
  de->ino = fe.cluster ? fe.cluster : index + 2;
  de->type = fe.directory ? T_DIR : T_FILE;
  safestrcpy(de->name, fe.name, sizeof(de->name));
  return 1;
}

static struct vnode_ops fat32_ops = {
  .stat = fat_vstat, .read = fat_vread, .readdir = fat_vreaddir,
  .write = fat_vwrite, .create = fat_vcreate, .truncate = fat_vtruncate,
  .rename = fat_vrename, .fsync = fat_vfsync,
};

static int
netfs_vstat(char *path, struct stat *st)
{
  return netfsstat(path, st);
}

static int
netfs_vread(char *path, uint64 off, void *dst, int n)
{
  return netfsread(path, off, dst, n);
}

static int
netfs_vreaddir(char *path, int index, void *arg)
{
  struct netfs_dirent ne;
  struct vfs_dirent *de = arg;
  int r = netfsreaddir(path, index, &ne);
  if(r <= 0)
    return r;
  memset(de, 0, sizeof(*de));
  de->ino = ne.ino;
  de->size = ne.size;
  de->type = ne.type;
  safestrcpy(de->name, ne.name, sizeof(de->name));
  return 1;
}

static struct vnode_ops netfs_ops = {
  .stat = netfs_vstat,
  .read = netfs_vread,
  .readdir = netfs_vreaddir,
};

static int
parsepid(char *s, int *pid, char **rest)
{
  int v = 0, digits = 0;
  while(*s >= '0' && *s <= '9'){
    v = v * 10 + *s++ - '0';
    digits++;
  }
  if(!digits || (*s != 0 && *s != '/'))
    return -1;
  *pid = v;
  *rest = s;
  return 0;
}

static int
streq(char *a, char *b)
{
  return strncmp(a, b, strlen(b) + 1) == 0;
}

static char*
state_name(enum procstate state)
{
  switch(state){
  case USED: return "used";
  case SLEEPING: return "sleeping";
  case RUNNABLE: return "runnable";
  case RUNNING: return "running";
  case ZOMBIE: return "zombie";
  default: return "unused";
  }
}

static struct proc*
findpid(int pid)
{
  for(struct proc *p = proc_head; p; p = p->next){
    acquire(&p->lock);
    if(p->state != UNUSED && p->pid == pid)
      return p;
    release(&p->lock);
  }
  return 0;
}

static char*
putstr(char *p, char *end, char *s)
{
  while(*s && p < end)
    *p++ = *s++;
  return p;
}

static char*
putnum(char *p, char *end, uint64 value)
{
  char tmp[24];
  int n = 0;
  do {
    tmp[n++] = '0' + value % 10;
    value /= 10;
  } while(value && n < sizeof(tmp));
  while(n && p < end)
    *p++ = tmp[--n];
  return p;
}

static char*
puthex(char *p, char *end, uint64 value)
{
  static char digits[] = "0123456789abcdef";
  p = putstr(p, end, "0x");
  for(int shift = 60; shift >= 0 && p < end; shift -= 4)
    *p++ = digits[(value >> shift) & 15];
  return p;
}

static char*
putmap(char *p, char *end, uint64 pa0, uint64 pa1, char *name)
{
  p = puthex(p, end, pa0);
  p = putstr(p, end, "-");
  p = puthex(p, end, pa1);
  p = putstr(p, end, " : ");
  p = putstr(p, end, name);
  p = putstr(p, end, "\n");
  return p;
}

static int
proc_memread(int iomem, uint64 off, void *dst, int n)
{
  char *buf = kalloc();
  char *p, *limit;
  int len;
  if(buf == 0)
    return -1;
  p = buf;
  limit = buf + PGSIZE;

  if(!iomem){
    p = putstr(p, limit, "MemTotal:       ");
    p = putnum(p, limit, PHYSTOP / 1024);
    p = putstr(p, limit, " kB\nMemFree:        ");
    p = putnum(p, limit, kfreepages() * PGSIZE / 1024);
    p = putstr(p, limit, " kB\nKernelTextPA:   ");
    p = puthex(p, limit, KERNPA);
    p = putstr(p, limit, "-");
    p = puthex(p, limit, V2P(etext) - 1);
    p = putstr(p, limit, "\nKernelDataPA:   ");
    p = puthex(p, limit, V2P(etext));
    p = putstr(p, limit, "-");
    p = puthex(p, limit, V2P(end) - 1);
    p = putstr(p, limit, "\nRamdiskPA:      ");
    p = puthex(p, limit, RAMDISK_PA);
    p = putstr(p, limit, "-");
    p = puthex(p, limit, RAMDISK_PA + RAMDISK_SIZE - 1);
    p = putstr(p, limit, "\n\nMMIO mappings (PA -> kernel VA):\n");
    p = putstr(p, limit, "BCM2837 peripherals  ");
    p = puthex(p, limit, PERIPHERAL_BASE_PA);
    p = putstr(p, limit, " -> ");
    p = puthex(p, limit, PERIPHERAL_BASE);
    p = putstr(p, limit, " size=16MB Device-nGnRnE XN\n");
    p = putstr(p, limit, "ARM local peripherals ");
    p = puthex(p, limit, LOCAL_BASE_PA);
    p = putstr(p, limit, " -> ");
    p = puthex(p, limit, LOCAL_BASE);
    p = putstr(p, limit, " size=4KB Device-nGnRnE XN\n");
    p = putstr(p, limit, "See /proc/iomem for controller subranges.\n");
  } else {
    p = putmap(p, limit, 0x00000000, PHYSTOP - 1, "System RAM");
    p = putmap(p, limit, KERNPA, V2P(etext) - 1, "  Kernel code");
    p = putmap(p, limit, V2P(etext), V2P(end) - 1, "  Kernel data/bss");
    p = putmap(p, limit, RAMDISK_PA, RAMDISK_PA + RAMDISK_SIZE - 1,
               "QEMU xv6 ramdisk");
    p = putmap(p, limit, 0x3f000000, 0x3fffffff,
               "BCM2837 peripheral MMIO window");
    p = putmap(p, limit, 0x3f003000, 0x3f003fff, "  BCM system timer");
    p = putmap(p, limit, 0x3f00b000, 0x3f00b3ff,
               "  legacy interrupt controller");
    p = putmap(p, limit, 0x3f00b880, 0x3f00b8bf, "  property mailbox");
    p = putmap(p, limit, 0x3f200000, 0x3f2000ff, "  GPIO controller");
    p = putmap(p, limit, 0x3f201000, 0x3f201fff, "  PL011 UART0 (unused)");
    p = putmap(p, limit, 0x3f215000, 0x3f215fff,
               "  AUX block / Mini UART");
    p = putmap(p, limit, 0x3f300000, 0x3f3000ff, "  Arasan EMMC/SD");
    p = putmap(p, limit, 0x3f980000, 0x3f9fffff, "  DWC2 USB host");
    p = putmap(p, limit, 0x40000000, 0x40000fff,
               "BCM2836/7 ARM-local interrupt/timer MMIO");
  }

  len = p - buf;
  if(off >= (uint64)len)
    n = 0;
  else {
    if(n > len - off)
      n = len - off;
    memmove(dst, buf + off, n);
  }
  kfree(buf);
  return n;
}

static int
proc_vstat(char *path, struct stat *st)
{
  int pid;
  char *rest;
  struct proc *p;
  memset(st, 0, sizeof(*st));
  st->dev = 3;
  st->nlink = 1;
  if(streq(path, "/")){
    st->ino = 1;
    st->type = T_DIR;
    return 0;
  }
  if(streq(path, "/meminfo") || streq(path, "/iomem") ||
     streq(path, "/devices") || streq(path, "/mounts") ||
     streq(path, "/cmdline")){
    st->ino = streq(path, "/meminfo") ? 2 : streq(path, "/iomem") ? 3 :
              streq(path, "/devices") ? 4 : streq(path, "/mounts") ? 5 : 6;
    st->type = T_FILE;
    st->size = 2048;
    return 0;
  }
  if(path[0] != '/' || parsepid(path + 1, &pid, &rest) < 0)
    return -1;
  p = findpid(pid);
  if(p == 0)
    return -1;
  if(*rest == 0 || streq(rest, "/")){
    st->ino = 1000 + pid;
    st->type = T_DIR;
  } else if(streq(rest, "/status")){
    st->ino = 2000 + pid;
    st->type = T_FILE;
    st->size = 128;
  } else {
    release(&p->lock);
    return -1;
  }
  release(&p->lock);
  return 0;
}

static int vfs_mounts_format(char *buf, int n);

static int
proc_vread(char *path, uint64 off, void *dst, int n)
{
  int pid, len;
  char *rest, buf[160], *q = buf, *end = buf + sizeof(buf);
  struct proc *p;
  if(streq(path, "/meminfo"))
    return proc_memread(0, off, dst, n);
  if(streq(path, "/iomem"))
    return proc_memread(1, off, dst, n);
  if(streq(path, "/devices") || streq(path, "/mounts") ||
     streq(path, "/cmdline")){
    char *page = kalloc();
    int len;
    if(page == 0)
      return -1;
    if(streq(path, "/devices"))
      len = device_format(page, PGSIZE);
    else if(streq(path, "/mounts"))
      len = vfs_mounts_format(page, PGSIZE);
    else {
      safestrcpy(page, cmdline_get_all(), PGSIZE - 1);
      len = strlen(page);
      page[len++] = '\n';
    }
    if(off >= (uint64)len)
      n = 0;
    else {
      if(n > len - off)
        n = len - off;
      memmove(dst, page + off, n);
    }
    kfree(page);
    return n;
  }
  if(path[0] != '/' || parsepid(path + 1, &pid, &rest) < 0 ||
     !streq(rest, "/status"))
    return -1;
  p = findpid(pid);
  if(p == 0)
    return -1;
  q = putstr(q, end, "Name:\t");
  q = putstr(q, end, p->name);
  q = putstr(q, end, "\nPid:\t");
  q = putnum(q, end, p->pid);
  q = putstr(q, end, "\nState:\t");
  q = putstr(q, end, state_name(p->state));
  q = putstr(q, end, "\nVmSize:\t");
  q = putnum(q, end, p->vm->sz);
  q = putstr(q, end, " bytes\nKilled:\t");
  q = putnum(q, end, p->killed);
  q = putstr(q, end, "\n");
  release(&p->lock);
  len = q - buf;
  if(off >= (uint64)len)
    return 0;
  if(n > len - off)
    n = len - off;
  memmove(dst, buf + off, n);
  return n;
}

static int
proc_vreaddir(char *path, int index, void *arg)
{
  struct vfs_dirent *de = arg;
  int pid;
  char *rest;
  struct proc *p;
  memset(de, 0, sizeof(*de));
  if(streq(path, "/")){
    if(index == 0){
      de->ino = 2;
      de->type = T_FILE;
      de->size = 2048;
      safestrcpy(de->name, "meminfo", sizeof(de->name));
      return 1;
    }
    if(index == 1){
      de->ino = 3;
      de->type = T_FILE;
      de->size = 2048;
      safestrcpy(de->name, "iomem", sizeof(de->name));
      return 1;
    }
    if(index == 2){
      de->ino = 4;
      de->type = T_FILE;
      de->size = 2048;
      safestrcpy(de->name, "devices", sizeof(de->name));
      return 1;
    }
    if(index == 3 || index == 4){
      de->ino = index == 3 ? 5 : 6;
      de->type = T_FILE;
      de->size = 2048;
      safestrcpy(de->name, index == 3 ? "mounts" : "cmdline",
                 sizeof(de->name));
      return 1;
    }
    int seen = 0;
    for(p = proc_head; p; p = p->next){
      acquire(&p->lock);
      if(p->state != UNUSED && seen++ == index - 5){
        pid = p->pid;
        release(&p->lock);
        de->ino = 1000 + pid;
        de->type = T_DIR;
        char tmp[16], *q = tmp + sizeof(tmp);
        *--q = 0;
        do { *--q = '0' + pid % 10; pid /= 10; } while(pid);
        safestrcpy(de->name, q, sizeof(de->name));
        return 1;
      }
      release(&p->lock);
    }
    return 0;
  }
  if(path[0] != '/' || parsepid(path + 1, &pid, &rest) < 0 ||
     (*rest != 0 && !streq(rest, "/")) || index != 0)
    return 0;
  p = findpid(pid);
  if(p == 0)
    return 0;
  release(&p->lock);
  de->ino = 2000 + pid;
  de->type = T_FILE;
  de->size = 128;
  safestrcpy(de->name, "status", sizeof(de->name));
  return 1;
}

static struct vnode_ops proc_ops = {
  .stat = proc_vstat,
  .read = proc_vread,
  .readdir = proc_vreaddir,
};

static int
mountops(char *path, struct vnode_ops *ops, int readonly, char *source)
{
  if(path[0] != '/')
    return -1;
  for(int i = 0; i < NMOUNT; i++)
    if(mounts[i].used && streq(mounts[i].path, path))
      return -1;
  for(int i = 0; i < NMOUNT; i++){
    if(!mounts[i].used){
      mounts[i].used = 1;
      mounts[i].ops = ops;
      mounts[i].readonly = readonly;
      safestrcpy(mounts[i].path, path, sizeof(mounts[i].path));
      safestrcpy(mounts[i].source, source, sizeof(mounts[i].source));
      return 0;
    }
  }
  return -1;
}

// /proc/mounts: xv6-inode mounts (fs.c), then the path-prefix mounts here.
static int
vfs_mounts_format(char *buf, int n)
{
  int len = fs_mounts_format(buf, n);
  char *p = buf + len, *e = buf + n;
  acquire(&vnodes.lock);
  for(int i = 0; i < NMOUNT; i++){
    if(!mounts[i].used)
      continue;
    struct vnode_ops *ops = mounts[i].ops;
    char *t = ops == &proc_ops ? "procfs" : ops == &fat32_ops ? "fat32" :
              ops == &ext2_ops ? "ext2" : ops == &netfs_ops ? "netfs" : "?";
    p = putstr(p, e, mounts[i].source);
    p = putstr(p, e, " ");
    p = putstr(p, e, mounts[i].path);
    p = putstr(p, e, " ");
    p = putstr(p, e, t);
    p = putstr(p, e, mounts[i].readonly ? " ro 0 0\n" : " rw 0 0\n");
  }
  release(&vnodes.lock);
  return p - buf;
}

int
vfsmount(char *source, char *path, char *fstype, int flags)
{
  struct vnode_ops *ops;
  int readonly = (flags & VFS_MOUNT_RDONLY) != 0;
  if(flags & ~VFS_MOUNT_RDONLY)
    return -1;
  if(streq(fstype, "procfs"))
    ops = &proc_ops;
  else if(streq(fstype, "fat32") && fat32ready())
    ops = &fat32_ops;
  else if(streq(fstype, "ext2") && ext2ready())
    ops = &ext2_ops;
  else if(streq(fstype, "netfs") && readonly &&
          netfs_configure(source) == 0)
    ops = &netfs_ops;
  else
    return -1;
  if(!readonly && ops != &fat32_ops && ops != &ext2_ops)
    return -1;
  acquire(&vnodes.lock);
  int r = mountops(path, ops, readonly, source);
  release(&vnodes.lock);
  if(r == 0)
    printf("vfs: mounted %s at %s %s\n", fstype, path,
           readonly ? "read-only" : "read-write");
  return r;
}

static struct vnode_ops*
findmount(char *path, char **relative, int *readonly)
{
  struct vfs_mount *best = 0;
  int bestlen = -1;
  for(int i = 0; i < NMOUNT; i++){
    int n;
    if(!mounts[i].used)
      continue;
    n = strlen(mounts[i].path);
    if(n == 1){
      // A filesystem mounted on "/" (ext2 root) matches every path, with the
      // lowest priority.  Paths under a native xv6 mount (/dev, devtmpfs)
      // stay in the inode world.
      if(bestlen < 0 && path[0] == '/' && !fs_native_covers(path)){
        best = &mounts[i];
        bestlen = 0;
      }
      continue;
    }
    if(n > bestlen && strncmp(path, mounts[i].path, n) == 0 &&
       (path[n] == 0 || path[n] == '/')){
      best = &mounts[i];
      bestlen = n;
    }
  }
  if(best == 0)
    return 0;
  *relative = path[bestlen] ? path + bestlen : "/";
  if(readonly)
    *readonly = best->readonly;
  return best->ops;
}

void
vfsinit(void)
{
  initlock(&vnodes.lock, "vnodes");
  memset(mounts, 0, sizeof(mounts));
  netfsinit();
  printf("vfs: native root rw; waiting for /etc/fstab mounts\n");
}

int
vfsopen(char *path, int omode, struct vnode **out)
{
  struct vnode *vn = 0;
  struct stat st;
  char *sub;
  int readonly, exists;
  int trace_create;
  struct vnode_ops *ops;
  ops = findmount(path, &sub, &readonly);
  if(ops == 0)
    return 0;
  trace_create = (omode & O_CREATE) && strncmp(path, "/boot/", 6) == 0;
  if(trace_create)
    printf("vfs: boot create begin path=%s sub=%s mode=%x\n",
           path, sub, omode);
  exists = ops->stat(sub, &st) == 0;
  if(trace_create)
    printf("vfs: boot create stat complete exists=%d\n", exists);
  int access = omode & (O_WRONLY | O_RDWR);
  if(access == O_RDONLY){
    if(!exists)
      return -1;
  } else {
    if(readonly || (access != O_WRONLY && access != O_RDWR) ||
       (omode & ~(O_WRONLY | O_RDWR | O_CREATE | O_TRUNC | O_NONBLOCK)) ||
       ops->write == 0 || (!exists && (!(omode & O_CREATE) ||
                                       ops->create == 0)))
      return -1;
    if(exists && ops == &fat32_ops && !(omode & O_TRUNC) &&
       st.type == T_FILE && st.size != 0)
      return -1;
    if(!exists){
      if(trace_create)
        printf("vfs: boot create directory-entry begin\n");
      if(ops->create(sub) < 0)
        return -1;
      if(trace_create)
        printf("vfs: boot create directory-entry complete\n");
    }
    if(omode & O_TRUNC){
      if(trace_create)
        printf("vfs: boot create truncate begin\n");
      if(ops->truncate == 0 || ops->truncate(sub, 0) < 0)
        return -1;
      if(trace_create)
        printf("vfs: boot create truncate complete\n");
    }
    if(trace_create)
      printf("vfs: boot create verify-stat begin\n");
    if(ops->stat(sub, &st) < 0 || st.type != T_FILE)
      return -1;
    if(trace_create)
      printf("vfs: boot create verify-stat complete\n");
  }
  acquire(&vnodes.lock);
  for(int i = 0; i < NFILE; i++){
    if(!vnodes.nodes[i].used){
      vn = &vnodes.nodes[i];
      vn->used = 1;
      vn->ref = 1;
      vn->type = st.type;
      vn->ops = ops;
      vn->readonly = readonly;
      safestrcpy(vn->path, sub, sizeof(vn->path));
      vn->has_handle = 0;
      vn->handle = 0;
      memset(&vn->fat_file, 0, sizeof(vn->fat_file));
      break;
    }
  }
  release(&vnodes.lock);
  if(vn == 0)
    return -1;
  if(vn->type == T_FILE && ops->open){
    if(ops->open(sub, &vn->handle) < 0){
      vfsclose(vn);
      return -1;
    }
    vn->has_handle = 1;
  }
  if(!readonly && omode != O_RDONLY && vn->ops == &fat32_ops){
    if(trace_create)
      printf("vfs: boot create openwrite begin\n");
    if(fat32openwrite(sub, &vn->fat_file) < 0){
      vfsclose(vn);
      return -1;
    }
    if(trace_create)
      printf("vfs: boot create openwrite complete\n");
  }
  *out = vn;
  return 1;
}

int
vfsrename(char *oldpath, char *newpath)
{
  char *oldsub, *newsub;
  int oldreadonly, newreadonly;
  struct vnode_ops *oldops = findmount(oldpath, &oldsub, &oldreadonly);
  struct vnode_ops *newops = findmount(newpath, &newsub, &newreadonly);
  if(oldops == 0 && newops == 0)
    return -1;
  if(oldops == 0 || oldops != newops || oldreadonly || newreadonly ||
     oldops->rename == 0)
    return -1;
  return oldops->rename(oldsub, newsub);
}

// -2 means the path belongs to the native root and the syscall should use the
// native inode implementation. -1 is an error below a VFS mount.
int
vfsunlink(char *path)
{
  char *sub;
  int readonly;
  struct vnode_ops *ops = findmount(path, &sub, &readonly);
  if(ops == 0) return -2;
  if(readonly || ops->unlink == 0) return -1;
  return ops->unlink(sub);
}

int
vfsmkdir(char *path)
{
  char *sub;
  int readonly;
  struct vnode_ops *ops = findmount(path, &sub, &readonly);
  if(ops == 0) return -2;
  if(readonly || ops->mkdir == 0) return -1;
  return ops->mkdir(sub);
}

int
vfsstatpath(char *path, struct stat *st)
{
  char *sub;
  struct vnode_ops *ops = findmount(path, &sub, 0);
  if(ops == 0) return -2;
  return ops->stat(sub, st);
}

int
vfsmounted(char *path)
{
  char *sub;
  return findmount(path, &sub, 0) != 0;
}

void
vfsclose(struct vnode *vn)
{
  void (*rel)(uint64) = 0;
  uint64 handle = 0;
  acquire(&vnodes.lock);
  if(vn == 0 || !vn->used || vn->ref < 1)
    panic("vfsclose");
  if(--vn->ref == 0){
    if(vn->has_handle){
      rel = vn->ops->release;
      handle = vn->handle;
      vn->has_handle = 0;
    }
    vn->used = 0;
    vn->ops = 0;
  }
  release(&vnodes.lock);
  // The backend may do disk I/O (orphan reclaim) under its own sleeplock.
  if(rel)
    rel(handle);
}

int
vfsstat(struct vnode *vn, struct stat *st)
{
  if(vn->has_handle && vn->ops->hstat)
    return vn->ops->hstat(vn->handle, st);
  return vn->ops->stat(vn->path, st);
}

int
vfsread(struct vnode *vn, int user_dst, uint64 dst, uint64 off, uint n)
{
  struct vfs_dirent vde;
  struct dirent de;
  char *page;
  int got, total = 0;

  if(vn->type == T_DIR){
    if(n < sizeof(de) || off % sizeof(de))
      return 0;
    got = vn->ops->readdir(vn->path, off / sizeof(de), &vde);
    if(got <= 0)
      return got;
    memset(&de, 0, sizeof(de));
    de.inum = vde.ino > 0xffff ? 0xffff : vde.ino;
    memmove(de.name, vde.name,
            strlen(vde.name) < DIRSIZ ? strlen(vde.name) : DIRSIZ);
    if(either_copyout(user_dst, dst, &de, sizeof(de)) < 0)
      return -1;
    return sizeof(de);
  }

  page = kalloc();
  if(page == 0)
    return -1;
  while(total < (int)n){
    int chunk = n - total;
    if(chunk > PGSIZE)
      chunk = PGSIZE;
    if(vn->has_handle && vn->ops->hread)
      got = vn->ops->hread(vn->handle, off + total, page, chunk);
    else
      got = vn->ops->read(vn->path, off + total, page, chunk);
    if(got <= 0)
      break;
    if(either_copyout(user_dst, dst + total, page, got) < 0){
      total = -1;
      break;
    }
    total += got;
    if(got < chunk)
      break;
  }
  kfree(page);
  return total;
}

int
vfswrite(struct vnode *vn, int user_src, uint64 src, uint64 off, uint n)
{
  if(vn == 0 || vn->readonly || vn->type != T_FILE ||
     vn->ops->write == 0 || off + n < off)
    return -1;
  if(vn->has_handle && vn->ops->hwrite)
    return vn->ops->hwrite(vn->handle, off, user_src, src, n);
  return vn->ops->write(vn->path, &vn->fat_file, off, user_src, src, n);
}

int
vfsftruncate(struct vnode *vn, uint64 size)
{
  if(vn == 0 || vn->readonly || vn->type != T_FILE ||
     vn->ops->truncate == 0)
    return -1;
  if(vn->has_handle && vn->ops->htruncate)
    return vn->ops->htruncate(vn->handle, size);
  return vn->ops->truncate(vn->path, size);
}

int
vfsfsync(struct vnode *vn)
{
  if(vn == 0 || vn->ops->fsync == 0)
    return -1;
  return vn->ops->fsync();
}
