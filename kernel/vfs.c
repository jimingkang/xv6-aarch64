// Small vnode layer.  The native xv6 inode filesystem remains the writable
// root; init mounts read-only backends named in /etc/fstab through mount(2).

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
#include "defs.h"
#include "vfs.h"
#include "device.h"

extern char etext[];
extern char end[];

struct vnode_ops {
  int (*stat)(char*, struct stat*);
  int (*read)(char*, uint64, void*, int);
  int (*readdir)(char*, int, void*);
};

struct vfs_dirent {
  uint ino;
  uint size;
  short type;
  char name[EXT2_NAME_MAX + 1];
};

struct vnode {
  int used;
  int ref;
  short type;
  char path[MAXPATH];
  struct vnode_ops *ops;
};

static struct {
  struct spinlock lock;
  struct vnode nodes[NFILE];
} vnodes;

#define NMOUNT 4
struct vfs_mount {
  int used;
  char path[MAXPATH];
  struct vnode_ops *ops;
};
static struct vfs_mount mounts[NMOUNT];

static int
ext2_vstat(char *path, struct stat *st)
{
  uint ino, size;
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
  .readdir = ext2_vreaddir,
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
  for(struct proc *p = proc; p < proc + NPROC; p++){
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
     streq(path, "/devices")){
    st->ino = streq(path, "/meminfo") ? 2 : streq(path, "/iomem") ? 3 : 4;
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
  if(streq(path, "/devices")){
    char *page = kalloc();
    int len;
    if(page == 0)
      return -1;
    len = device_format(page, PGSIZE);
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
  q = putnum(q, end, p->sz);
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
    int seen = 0;
    for(p = proc; p < proc + NPROC; p++){
      acquire(&p->lock);
      if(p->state != UNUSED && seen++ == index - 3){
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
mountops(char *path, struct vnode_ops *ops)
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
      safestrcpy(mounts[i].path, path, sizeof(mounts[i].path));
      return 0;
    }
  }
  return -1;
}

int
vfsmount(char *path, char *fstype)
{
  struct vnode_ops *ops;
  if(streq(fstype, "procfs"))
    ops = &proc_ops;
  else if(streq(fstype, "ext2") && ext2ready())
    ops = &ext2_ops;
  else
    return -1;
  acquire(&vnodes.lock);
  int r = mountops(path, ops);
  release(&vnodes.lock);
  if(r == 0)
    printf("vfs: mounted %s at %s read-only\n", fstype, path);
  return r;
}

static struct vnode_ops*
findmount(char *path, char **relative)
{
  struct vfs_mount *best = 0;
  int bestlen = -1;
  for(int i = 0; i < NMOUNT; i++){
    int n;
    if(!mounts[i].used)
      continue;
    n = strlen(mounts[i].path);
    if(n > bestlen && strncmp(path, mounts[i].path, n) == 0 &&
       (path[n] == 0 || path[n] == '/')){
      best = &mounts[i];
      bestlen = n;
    }
  }
  if(best == 0)
    return 0;
  *relative = path[bestlen] ? path + bestlen : "/";
  return best->ops;
}

void
vfsinit(void)
{
  initlock(&vnodes.lock, "vnodes");
  memset(mounts, 0, sizeof(mounts));
  printf("vfs: native root rw; waiting for /etc/fstab mounts\n");
}

int
vfsopen(char *path, int omode, struct vnode **out)
{
  struct vnode *vn = 0;
  struct stat st;
  char *sub;
  struct vnode_ops *ops;
  ops = findmount(path, &sub);
  if(ops == 0)
    return 0;
  if(omode != O_RDONLY || ops->stat(sub, &st) < 0)
    return -1;
  acquire(&vnodes.lock);
  for(int i = 0; i < NFILE; i++){
    if(!vnodes.nodes[i].used){
      vn = &vnodes.nodes[i];
      vn->used = 1;
      vn->ref = 1;
      vn->type = st.type;
      vn->ops = ops;
      safestrcpy(vn->path, sub, sizeof(vn->path));
      break;
    }
  }
  release(&vnodes.lock);
  if(vn == 0)
    return -1;
  *out = vn;
  return 1;
}

void
vfsclose(struct vnode *vn)
{
  acquire(&vnodes.lock);
  if(vn == 0 || !vn->used || vn->ref < 1)
    panic("vfsclose");
  if(--vn->ref == 0){
    vn->used = 0;
    vn->ops = 0;
  }
  release(&vnodes.lock);
}

int
vfsstat(struct vnode *vn, struct stat *st)
{
  return vn->ops->stat(vn->path, st);
}

int
vfsread(struct vnode *vn, int user_dst, uint64 dst, uint off, uint n)
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
