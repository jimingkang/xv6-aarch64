// Virtual filesystem glue: path-based filesystems (FAT32, procfs, netfs)
// as VFS inodes, and mount(2).  The VFS core (super blocks, inode
// operations, the mount table, path lookup) is in fs.c; ext2's inode
// operations are in ext2.c.

#include <kernel/types.h>
#include <kernel/param.h>
#include "memlayout.h"
#include "aarch64.h"
#include <kernel/spinlock.h>
#include <kernel/sleeplock.h>
#include <kernel/proc.h>
#include <kernel/fs.h>
#include <kernel/fcntl.h>
#include <kernel/stat.h>
#include <kernel/file.h>
#include <kernel/ext2.h>
#include <kernel/fat32.h>
#include <kernel/netfs.h>
#include <kernel/blkdev.h>
#include <kernel/defs.h>
#include <kernel/vfs.h>
#include <kernel/device.h>
#include <kernel/kobject.h>
#include <kernel/module.h>
#include <kernel/fs_trace.h>

extern char etext[];
extern char end[];

struct vfs_dirent {
  uint ino;
  uint64 size;
  short type;
  char name[EXT2_NAME_MAX + 1];
};

static int streq(char *a, char *b);
static uint pathhash(char *path);

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


// ---------------------------------------------------------------------------
// sysfs: the kobject tree read as a filesystem.  procfs above prints one
// hand-written snapshot per file; sysfs has no such list -- every file is a
// (kobject, attribute) pair that already exists in the driver core, and the
// walker below only turns that tree into directory entries and calls the
// attribute's show/store.  Nothing here names a device or a bus: adding a
// driver that registers a kobject makes new /sys files appear with no change
// to this code, exactly as in Linux's drivers/base/core + fs/sysfs.

// One resolved path: a directory (a kobject, attr 0) or a file (attr set).
struct sys_target {
  struct kobject   *kobj;
  struct attribute *attr;
};

// 0 = no such object, 1 = directory, 2 = attribute file.  Walk "/"-separated
// components from the sysfs root through child kobjects; the final component
// may instead name one of the current object's attribute files.
static int
sys_resolve(char *path, struct sys_target *out)
{
  struct kobject *cur = kobject_sys_root();
  char *s = path, *e, name[KOBJ_NAME_MAX];

  while(*s == '/')
    s++;
  while(*s){
    e = s;
    while(*e && *e != '/')
      e++;
    if(e - s >= (int) sizeof(name))
      return 0;
    memmove(name, s, e - s);
    name[e - s] = 0;
    s = *e ? e + 1 : e;
    if(*s == 0){                             // last component: child or attr?
      struct kobject *child = kobject_child_by_name(cur, name);
      if(child){
        // A sysfs symlink (/sys/class/<class>/<dev>) resolves to its target, so
        // stat/readdir/read of the link path act on the object it points at --
        // exactly Linux's stat-follows-symlink semantics.
        out->kobj = kobject_follow_link(child);
        out->attr = 0;
        return 1;
      }
      struct attribute *a = kobject_find_attr(cur, name);
      if(a == 0)
        return 0;
      out->kobj = cur;
      out->attr = a;
      return 2;
    }
    cur = kobject_child_by_name(cur, name);
    if(cur == 0)
      return 0;
    cur = kobject_follow_link(cur);          // follow intermediate links
  }
  out->kobj = cur;                            // path "/" or a bare directory
  out->attr = 0;
  return 1;
}

static int
sys_vstat(char *path, struct stat *st)
{
  struct sys_target t;
  int r = sys_resolve(path, &t);
  if(r == 0)
    return -1;
  memset(st, 0, sizeof(*st));
  st->dev = SYSFS_DEV;
  st->nlink = 1;
  st->ino = pathhash(path);
  if(r == 1){
    st->type = T_DIR;
    st->size = 0;
  } else {
    st->type = T_FILE;
    st->size = 4096;                          // Linux reports a page for attrs
  }
  return 0;
}

static int
sys_vread(char *path, uint64 off, void *dst, int n)
{
  struct sys_target t;
  struct kobj_type *ktype;
  char *page;
  int len;

  if(sys_resolve(path, &t) != 2)
    return -1;
  ktype = t.kobj->ktype;
  if(ktype == 0 || ktype->sysfs_ops == 0 || ktype->sysfs_ops->show == 0)
    return -1;
  if((page = kalloc()) == 0)
    return -1;
  len = ktype->sysfs_ops->show(t.kobj, t.attr, page, PGSIZE);
  if(len < 0)
    len = 0;
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

// write(2) to an attribute file: copy the user bytes into a page, then hand
// them to the attribute's store().  Read-only attributes (mode without a
// write bit) reject the write, as sysfs does with -EACCES.
static int
sys_vwrite(char *path, struct fat32_file *file, uint64 off, int user_src,
           uint64 src, int n)
{
  struct sys_target t;
  struct kobj_type *ktype;
  char *page;
  int len;

  (void) file;
  (void) off;
  if(n <= 0 || n > PGSIZE - 1)
    return -1;
  if(sys_resolve(path, &t) != 2)
    return -1;
  if((t.attr->mode & 0200) == 0)
    return -1;
  ktype = t.kobj->ktype;
  if(ktype == 0 || ktype->sysfs_ops == 0 || ktype->sysfs_ops->store == 0)
    return -1;
  if((page = kalloc()) == 0)
    return -1;
  if(either_copyin(page, user_src, src, n) < 0){
    kfree(page);
    return -1;
  }
  page[n] = 0;
  len = ktype->sysfs_ops->store(t.kobj, t.attr, page, n);
  kfree(page);
  return len;
}

// sysfs attribute files ignore truncation: opening with O_TRUNC (what the
// shell's ">" does) must succeed without erasing anything, exactly as Linux
// sysfs treats a write to an attribute as a store rather than a data append.
static int
sys_vtruncate(char *path, uint64 size)
{
  struct sys_target t;
  return sys_resolve(path, &t) == 2 ? 0 : -1;
}

// A sysfs directory holds the object's child kobjects first, then its
// attribute files -- the same order Linux's sysfs readdir yields.
static int
sys_vreaddir(char *path, int index, void *arg)
{
  struct sys_target t;
  struct vfs_dirent *de = arg;
  struct kobject *child;
  struct attribute *a;
  int nchild;

  memset(de, 0, sizeof(*de));
  if(sys_resolve(path, &t) != 1)
    return 0;
  nchild = kobject_child_count(t.kobj);
  if(index < nchild){
    if((child = kobject_child_at(t.kobj, index)) == 0)
      return 0;
    de->ino = pathhash(child->name);
    de->type = T_DIR;
    safestrcpy(de->name, child->name, sizeof(de->name));
    return 1;
  }
  if((a = kobject_attr_at(t.kobj, index - nchild)) == 0)
    return 0;
  de->ino = pathhash(a->name);
  de->type = T_FILE;
  de->size = 4096;
  safestrcpy(de->name, a->name, sizeof(de->name));
  return 1;
}

// Resolve `path` the way readlink(2) does: succeed only when the final
// component is a sysfs symlink, returning the object it points at.  Unlike
// sys_resolve, the last component is *not* dereferenced -- the point is to
// report the link, not to walk through it.  Intermediate components still
// follow links, matching how the kernel resolves each directory element.
static int
sys_link_target(char *path, struct kobject **out)
{
  struct kobject *cur = kobject_sys_root();
  struct kobject *child;
  char *s = path, *e, name[KOBJ_NAME_MAX];

  while(*s == '/')
    s++;
  while(*s){
    e = s;
    while(*e && *e != '/')
      e++;
    if(e - s >= (int) sizeof(name))
      return -1;
    memmove(name, s, e - s);
    name[e - s] = 0;
    s = *e ? e + 1 : e;
    if((child = kobject_child_by_name(cur, name)) == 0)
      return -1;
    if(*s == 0){                             // final component
      if(child->link == 0)
        return -1;                           // not a symlink
      *out = child->link;
      return 0;
    }
    cur = kobject_follow_link(child);
  }
  return -1;                                 // path named a directory or root
}

// readlink(2) on a sysfs link: emit the target's path, absolute under the
// conventional /sys mount point.  Linux returns a path relative to the link;
// a root-relative absolute form is the usable equivalent here, since the tree
// root itself renders as "".
static int
sys_vreadlink(char *path, char *buf, int bufsz)
{
  struct kobject *target;
  char inner[MAXPATH];
  const char *pre = "/sys";
  int i = 0;

  if(sys_link_target(path, &target) < 0)
    return -1;
  if(kobject_path(target, inner, sizeof(inner)) == 0)
    return -1;
  for(const char *c = pre; *c; c++){
    if(i + 1 >= bufsz)
      return -1;
    buf[i++] = *c;
  }
  for(char *c = inner; *c; c++){
    if(i + 1 >= bufsz)
      return -1;
    buf[i++] = *c;
  }
  buf[i] = 0;
  return i;
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
     streq(path, "/cmdline") || streq(path, "/modules")){
    st->ino = streq(path, "/meminfo") ? 2 : streq(path, "/iomem") ? 3 :
              streq(path, "/devices") ? 4 : streq(path, "/mounts") ? 5 :
              streq(path, "/cmdline") ? 6 : 7;
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
  if(streq(path, "/devices") || streq(path, "/mounts") ||
     streq(path, "/cmdline") || streq(path, "/modules")){
    char *page = kalloc();
    int len;
    if(page == 0)
      return -1;
    if(streq(path, "/devices"))
      len = device_format(page, PGSIZE);
    else if(streq(path, "/mounts"))
      len = fs_mounts_format(page, PGSIZE);
    else if(streq(path, "/modules"))
      len = module_format(page, PGSIZE);
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
    if(index == 5){
      de->ino = 7;
      de->type = T_FILE;
      de->size = 2048;
      safestrcpy(de->name, "modules", sizeof(de->name));
      return 1;
    }
    int seen = 0;
    for(p = proc_head; p; p = p->next){
      acquire(&p->lock);
      if(p->state != UNUSED && seen++ == index - 6){
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


// ---------------------------------------------------------------------------
// Path-based filesystems.  FAT32 (no inode numbers on disk), procfs
// (synthetic) and netfs (a network protocol keyed by path) name their
// objects by path.  The VFS still gives each object a struct inode:
// ip->path is its path inside the filesystem and ip->inum a hash of it, so
// these filesystems are mounted, looked up and opened exactly like xv6fs
// and ext2.  This is the role Linux's simple_* helpers play for small
// in-kernel filesystems.

struct pathfs_ops {
  int (*stat)(char*, struct stat*);
  int (*read)(char*, uint64, void*, int);
  int (*write)(char*, struct fat32_file*, uint64, int, uint64, int);
  int (*readdir)(char*, int, void*);
  int (*create)(char*);
  int (*mkdir)(char*);
  int (*unlink)(char*);
  int (*rename)(char*, char*);
  int (*truncate)(char*, uint64);
  int (*openwrite)(char*, struct fat32_file*);
  int (*fsync)(void);
  int (*readlink)(char*, char*, int);
};

// FNV-1a, 31 bits, never 0 (ls hides inode 0).
static uint
pathhash(char *path)
{
  uint h = 2166136261u;
  for(; *path; path++){
    h ^= (uchar)*path;
    h *= 16777619u;
  }
  h &= 0x7fffffff;              // ls prints inode numbers as int
  return h ? h : 1;
}

static struct pathfs_ops*
pops(struct inode *ip)
{
  return getsuper(ip->dev)->priv;
}

// out = dir/name, with "." and ".." resolved textually inside the
// filesystem (".." of its root stays at the root).
static int
pathjoin(char *out, char *dir, char *name)
{
  if(streq(name, ".")){
    safestrcpy(out, dir, MAXPATH);
    return 0;
  }
  if(streq(name, "..")){
    safestrcpy(out, dir, MAXPATH);
    int n = strlen(out);
    while(n > 1 && out[n - 1] != '/')
      n--;
    if(n > 1)
      n--;
    out[n] = 0;
    return 0;
  }
  int a = strlen(dir), b = strlen(name);
  if(a + 1 + b + 1 > MAXPATH)
    return -1;
  memmove(out, dir, a);
  if(a > 1)
    out[a++] = '/';
  memmove(out + a, name, b + 1);
  return 0;
}

static int
pfs_read_inode(struct inode *ip)
{
  struct stat st;
  if(pops(ip)->stat(ip->path, &st) < 0)
    return -1;
  ip->type = st.type;
  ip->size = st.size;
  ip->nlink = st.nlink ? st.nlink : 1;
  ip->major = ip->minor = 0;
  memset(ip->fsdata, 0, sizeof(ip->fsdata));
  return 0;
}

static struct inode*
pfs_lookup(struct inode *dp, char *name)
{
  char p[MAXPATH];
  struct stat st;
  if(pathjoin(p, dp->path, name) < 0 || pops(dp)->stat(p, &st) < 0)
    return 0;
  return iget_path(dp->dev, pathhash(p), p);
}

static struct inode*
pfs_create(struct inode *dp, char *name, short type, short major, short minor)
{
  char p[MAXPATH];
  struct stat st;
  struct pathfs_ops *o = pops(dp);
  struct inode *ip;
  uint dev = dp->dev;

  int bad = pathjoin(p, dp->path, name) < 0;
  iput(dp);
  if(bad)
    return 0;
  if(o->stat(p, &st) == 0){
    if(type != T_FILE || st.type != T_FILE)
      return 0;
  } else if(type == T_FILE){
    if(o->create == 0 || o->create(p) < 0)
      return 0;
  } else if(type == T_DIR){
    if(o->mkdir == 0 || o->mkdir(p) < 0)
      return 0;
  } else {
    return 0;
  }
  ip = iget_path(dev, pathhash(p), p);
  ilock(ip);
  return ip;
}

static int
pfs_unlink(struct inode *dp, char *name)
{
  char p[MAXPATH];
  if(pops(dp)->unlink == 0 || pathjoin(p, dp->path, name) < 0)
    return -1;
  return pops(dp)->unlink(p);
}

static int
pfs_rename(struct inode *odp, char *oname, struct inode *ndp, char *nname)
{
  char a[MAXPATH], b[MAXPATH];
  if(pops(odp)->rename == 0 || pathjoin(a, odp->path, oname) < 0 ||
     pathjoin(b, ndp->path, nname) < 0)
    return -1;
  return pops(odp)->rename(a, b);
}

static int
pfs_readdir(struct inode *dp, int index, char *name, uint *ino)
{
  struct vfs_dirent vde;
  int r = pops(dp)->readdir(dp->path, index, &vde);
  if(r > 0){
    safestrcpy(name, vde.name, NAMEMAX);
    *ino = vde.ino;
  }
  return r;
}

static int
pfs_read(struct inode *ip, int user_dst, uint64 dst, uint64 off, uint n)
{
  if(ip->type == T_DIR)
    return vfs_dir_read(ip, user_dst, dst, off, n, pfs_readdir);
  char *page = kalloc();
  int total = 0;
  if(page == 0)
    return -1;
  while(total < (int)n){
    int chunk = n - total > PGSIZE ? PGSIZE : n - total;
    int got = pops(ip)->read(ip->path, off + total, page, chunk);
    if(got <= 0)
      break;
    if(either_copyout(user_dst, dst + total, page, got) < 0){
      total = total ? total : -1;
      break;
    }
    total += got;
    if(got < chunk)
      break;
  }
  kfree(page);
  return total;
}

static void
pfs_refresh_size(struct inode *ip)
{
  struct stat st;
  if(pops(ip)->stat(ip->path, &st) == 0)
    ip->size = st.size;
}

static int
pfs_write(struct inode *ip, int user_src, uint64 src, uint64 off, uint n)
{
  if(ip->type != T_FILE || pops(ip)->write == 0)
    return -1;
  int r = pops(ip)->write(ip->path, (struct fat32_file *)ip->fsdata, off,
                          user_src, src, n);
  pfs_refresh_size(ip);
  return r;
}

static int
pfs_truncate(struct inode *ip, uint64 size)
{
  if(ip->type != T_FILE || pops(ip)->truncate == 0 ||
     pops(ip)->truncate(ip->path, size) < 0)
    return -1;
  pfs_refresh_size(ip);
  return 0;
}

// FAT32 cannot overwrite a file in place: a non-empty file can only be
// opened for writing together with O_TRUNC (sys_open truncates first).
// Writers need a FAT write handle, kept in the inode.
static int
pfs_open(struct inode *ip, int omode)
{
  struct pathfs_ops *o = pops(ip);
  if((omode & (O_WRONLY | O_RDWR)) == 0 || ip->type != T_FILE)
    return 0;
  if(o->write == 0)
    return -1;
  if(o->openwrite){
    if(ip->size != 0)
      return -1;
    if(o->openwrite(ip->path, (struct fat32_file *)ip->fsdata) < 0)
      return -1;
  }
  return 0;
}

static int
pfs_fsync(struct inode *ip)
{
  return pops(ip)->fsync ? pops(ip)->fsync() : 0;
}

static int
pfs_readlink(struct inode *ip, char *buf, int bufsz)
{
  struct pathfs_ops *o = pops(ip);
  return o->readlink ? o->readlink(ip->path, buf, bufsz) : -1;
}

static struct inode_ops pathfs_iops = {
  .read_inode = pfs_read_inode,
  .lookup = pfs_lookup,
  .create = pfs_create,
  .unlink = pfs_unlink,
  .rename = pfs_rename,
  .read = pfs_read,
  .write = pfs_write,
  .truncate = pfs_truncate,
  .open = pfs_open,
  .fsync = pfs_fsync,
  .readlink = pfs_readlink,
};

static struct pathfs_ops fat32_pops = {
  .stat = fat_vstat, .read = fat_vread, .write = fat_vwrite,
  .readdir = fat_vreaddir, .create = fat_vcreate, .truncate = fat_vtruncate,
  .rename = fat_vrename, .openwrite = fat32openwrite, .fsync = fat_vfsync,
};

static struct pathfs_ops proc_pops = {
  .stat = proc_vstat, .read = proc_vread, .readdir = proc_vreaddir,
};

static struct pathfs_ops netfs_pops = {
  .stat = netfs_vstat, .read = netfs_vread, .readdir = netfs_vreaddir,
};

// sysfs is mounted read-write so writable attributes can store(); per-file
// read-only-ness is enforced by each attribute's mode bits in sys_vwrite,
// which is exactly how Linux separates the mount from sysfs file permissions.
static struct pathfs_ops sys_pops = {
  .stat = sys_vstat, .read = sys_vread, .write = sys_vwrite,
  .readdir = sys_vreaddir, .truncate = sys_vtruncate,
  .readlink = sys_vreadlink,
};

// ---------------------------------------------------------------------------
// mount(2): every filesystem goes into the one mount table in fs.c.

// The block device that holds the ext2 partition (mmcblk0pN).
static int
ext2_blkdev(void)
{
  uint32 lba = ext2_part_lba();
  for(int i = 0; i < NBLKDEV; i++){
    struct blkdev *bd = blkdev_at(i);
    if(bd && bd->dev >= BLKDEV_MMC_PART(1) && bd->dev <= BLKDEV_MMC_PART(4) &&
       bd->start == lba)
      return bd->dev;
  }
  return -1;
}

int
vfsmount(char *source, char *target, char *fstype, int flags)
{
  int readonly = (flags & VFS_MOUNT_RDONLY) != 0;
  int dev;
  struct inode *mp;

  VFSTRACE("vfsmount: resolve source=%s target=%s type=%s flags=%x\n",
           source, target, fstype, flags);

  if(flags & ~VFS_MOUNT_RDONLY)
    return -1;
  if(streq(fstype, "procfs")){
    dev = PROCFS_DEV;
    readonly = 1;
    fs_super_register(dev, "procfs", &pathfs_iops, pathhash("/"), 1, 0, 1,
                      &proc_pops);
  } else if(streq(fstype, "sysfs")){
    dev = SYSFS_DEV;
    fs_super_register(dev, "sysfs", &pathfs_iops, pathhash("/"), readonly,
                      0, 1, &sys_pops);
  } else if(streq(fstype, "fat32")){
    if(!fat32ready())
      return -1;
    dev = FATFS_DEV;
    fs_super_register(dev, "fat32", &pathfs_iops, pathhash("/"), readonly,
                      0, 1, &fat32_pops);
  } else if(streq(fstype, "ext2")){
    if(!ext2ready() || (dev = ext2_blkdev()) < 0)
      return -1;
    if(getsuper(dev) == 0 && ext2_register_super(dev, readonly) < 0)
      return -1;
  } else if(streq(fstype, "netfs")){
    if(!readonly || netfs_configure(source) < 0)
      return -1;
    dev = NETFS_DEV;
    fs_super_register(dev, "netfs", &pathfs_iops, pathhash("/"), 1, 0, 1,
                      &netfs_pops);
  } else {
    return -1;
  }

  begin_op();
  if((mp = namei(target)) == 0){
    VFSTRACE("vfsmount: target lookup failed target=%s\n", target);
    end_op();
    return -1;
  }
  int r = fs_mount(mp, dev, fstype, source, target);
  if(r < 0)
    iput(mp);
  end_op();
  if(r == 0)
    printf("vfs: mounted %s at %s %s\n", fstype, target,
           readonly ? "read-only" : "read-write");
  else
    printf("vfs: cannot mount %s at %s (busy or already mounted)\n",
           fstype, target);
  VFSTRACE("vfsmount: type=%s target=%s dev=%d result=%d\n",
           fstype, target, dev, r);
  return r;
}

void
vfsinit(void)
{
  netfsinit();
  printf("vfs: one mount table for all filesystems\n");
}

// Order 6: builds the mount table once every filesystem driver is ready.
late_initcall_ord("vfs", vfsinit, 6);
