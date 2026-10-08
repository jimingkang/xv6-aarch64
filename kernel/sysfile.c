//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "aarch64.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"
#include "device.h"
#include "ext2.h"
#include "epoll.h"
#include "vfs.h"
#include "socket.h"

static uint16
net16(uint16 value)
{
  return (value << 8) | (value >> 8);
}

// Rewrite path in place as a normalized absolute path.  A relative path is
// joined to the process's cwdpath; "." and ".." are resolved textually, which
// is exact because xv6 has no symbolic links.  Every path-taking syscall calls
// this first, so the native namei() and the VFS mount table both see the same
// absolute name and relative paths work inside /mnt/ext2, /boot or /proc.
static int
abspath(char *path)
{
  char tmp[MAXPATH];
  struct proc *p = myproc();
  int n;

  if(path[0] == 0)
    return -1;
  if(path[0] == '/'){
    safestrcpy(tmp, path, sizeof(tmp));
  } else {
    int a = strlen(p->cwdpath), b = strlen(path);
    if(a + 1 + b + 1 > MAXPATH)
      return -1;
    memmove(tmp, p->cwdpath, a);
    tmp[a] = '/';
    memmove(tmp + a + 1, path, b + 1);
  }

  path[0] = '/';
  n = 1;
  for(char *s = tmp; *s; ){
    while(*s == '/')
      s++;
    if(*s == 0)
      break;
    char *e = s;
    while(*e && *e != '/')
      e++;
    int len = e - s;
    if(len == 1 && s[0] == '.'){
      // stay
    } else if(len == 2 && s[0] == '.' && s[1] == '.'){
      if(n > 1){                       // drop the last component
        n--;
        while(n > 1 && path[n - 1] != '/')
          n--;
        if(n > 1)
          n--;
      }
    } else {
      if(n + (n > 1) + len >= MAXPATH)
        return -1;
      if(n > 1)
        path[n++] = '/';
      memmove(path + n, s, len);
      n += len;
    }
    s = e;
  }
  path[n] = 0;
  return 0;
}

uint64
sys_mount(void)
{
  char source[MAXPATH], target[MAXPATH], fstype[16];
  int flags;
  if(argstr(0, source, sizeof(source)) < 0 ||
     argstr(1, target, sizeof(target)) < 0 ||
     argstr(2, fstype, sizeof(fstype)) < 0 || argint(3, &flags) < 0 ||
     abspath(target) < 0)
    return -1;
  return vfsmount(source, target, fstype, flags);
}

uint64
sys_rename(void)
{
  char oldpath[MAXPATH], newpath[MAXPATH];
  if(argstr(0, oldpath, sizeof(oldpath)) < 0 ||
     argstr(1, newpath, sizeof(newpath)) < 0 ||
     abspath(oldpath) < 0 || abspath(newpath) < 0)
    return -1;
  char oname[NAMEMAX], nname[NAMEMAX];
  struct inode *odp, *ndp, *t;
  int r = -1;
  begin_op();
  if((odp = nameiparent(oldpath, oname)) == 0){
    end_op();
    return -1;
  }
  if((ndp = nameiparent(newpath, nname)) == 0){
    iput(odp);
    end_op();
    return -1;
  }
  ilock(odp);
  t = odp->type == T_DIR ? odp->iop->lookup(odp, oname) : 0;
  iunlock(odp);
  if(t){
    int busy = fs_is_mountpoint(t);   // EBUSY: something is mounted on it
    iput(t);
    if(!busy)
      r = fs_rename(odp, oname, ndp, nname);
  }
  iput(ndp);
  iput(odp);
  end_op();
  return r;
}

uint64
sys_ext2read(void)
{
  char path[MAXPATH];
  uint64 dst, off;
  int n, got;
  void *page;
  if(argstr(0, path, MAXPATH) < 0 || argaddr(1, &dst) < 0 ||
     argint(2, &n) < 0 || argaddr(3, &off) < 0 || n < 0 || n > PGSIZE)
    return -1;
  if((page = kalloc()) == 0) return -1;
  got = ext2readfile(path, off, page, n);
  if(got > 0 && copyout(myproc()->vm->pagetable, dst, page, got) < 0) got = -1;
  kfree(page);
  return got;
}

uint64
sys_ext2readdir(void)
{
  char path[MAXPATH];
  int index, r;
  uint64 dst;
  struct ext2_user_dirent de;
  if(argstr(0, path, MAXPATH) < 0 || argint(1, &index) < 0 ||
     argaddr(2, &dst) < 0)
    return -1;
  r = ext2readdir(path, index, &de);
  if(r > 0 && copyout(myproc()->vm->pagetable, dst, (char*)&de, sizeof(de)) < 0)
    return -1;
  return r;
}

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  if(argint(n, &fd) < 0)
    return -1;
  if(fd < 0 || fd >= NOFILE || (f=myproc()->ofile[fd]) == 0)
    return -1;
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}

uint64
sys_fsync(void)
{
  struct file *f;
  if(argfd(0, 0, &f) < 0) return -1;
  return filefsync(f, 0);
}

uint64
sys_fdatasync(void)
{
  struct file *f;
  if(argfd(0, 0, &f) < 0) return -1;
  return filefsync(f, 1);
}

uint64
sys_ftruncate(void)
{
  struct file *f;
  uint64 size;
  if(argfd(0, 0, &f) < 0 || argaddr(1, &size) < 0)
    return -1;
  return filetruncate(f, size);
}

uint64
sys_pread(void)
{
  struct file *f;
  uint64 dst, off;
  int n;
  if(argfd(0, 0, &f) < 0 || argaddr(1, &dst) < 0 ||
     argint(2, &n) < 0 || argaddr(3, &off) < 0)
    return -1;
  return filepread(f, dst, n, off);
}

uint64
sys_pwrite(void)
{
  struct file *f;
  uint64 src, off;
  int n;
  if(argfd(0, 0, &f) < 0 || argaddr(1, &src) < 0 ||
     argint(2, &n) < 0 || argaddr(3, &off) < 0)
    return -1;
  return filepwrite(f, src, n, off);
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *p = myproc();

  for(fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd] == 0){
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

// Create a POSIX-shaped TCP socket.  bind() records the port and listen()
// creates the network-stack listener, so socket descriptors participate in
// the same struct file/read/write/close/epoll machinery as other descriptors.
uint64
sys_socket(void)
{
  int domain, type, protocol, fd;
  struct file *f;

  if(argint(0, &domain) < 0 || argint(1, &type) < 0 ||
     argint(2, &protocol) < 0 || domain != AF_INET || type != SOCK_STREAM ||
     (protocol != 0 && protocol != IPPROTO_TCP))
    return -1;
  if((f = filealloc()) == 0)
    return -1;
  f->type = FD_SOCKET;
  f->readable = 1;
  f->writable = 1;
  f->socket = -1;
  if((fd = fdalloc(f)) < 0){
    fileclose(f);
    return -1;
  }
  return fd;
}

uint64
sys_bind(void)
{
  struct file *f;
  struct sockaddr_in address;
  uint64 user_address;
  int length;

  if(argfd(0, 0, &f) < 0 || f->type != FD_SOCKET || f->socket >= 0 ||
     argaddr(1, &user_address) < 0 || argint(2, &length) < 0 ||
     length < sizeof(address) ||
     copyin(myproc()->vm->pagetable, (char *)&address, user_address,
            sizeof(address)) < 0 || address.sin_family != AF_INET)
    return -1;
  f->socket_port = net16(address.sin_port);
  if(f->socket_port <= 0 || f->socket_port > 65535)
    return -1;
  return 0;
}

uint64
sys_listen(void)
{
  struct file *f;
  int backlog, handle;

  if(argfd(0, 0, &f) < 0 || f->type != FD_SOCKET || f->socket >= 0 ||
     f->socket_port == 0 || argint(1, &backlog) < 0 || backlog <= 0)
    return -1;
  if((handle = net_tcp_listen(f->socket_port, backlog)) < 0)
    return -1;
  f->socket = handle;
  f->socket_backlog = backlog;
  return 0;
}

uint64
sys_lseek(void)
{
  struct file *f;
  uint64 raw_offset;
  int whence;
  long offset;
  uint64 base, result;

  if(argfd(0, 0, &f) < 0 || argaddr(1, &raw_offset) < 0 ||
     argint(2, &whence) < 0)
    return -1;
  offset = (long)raw_offset;
  int blk = f->type == FD_DEVICE && f->major == BLOCKDEV;
  if(f->type != FD_INODE && !blk)
    return -1;
  if(whence == 0){
    base = 0;
  } else if(whence == 1){
    base = f->off;
  } else if(whence == 2){
    if(blk){
      if((base = blkdev_size(f->ip->minor)) == 0)
        return -1;
    } else {
      ilock(f->ip);
      base = f->ip->size;
      iunlock(f->ip);
    }
  } else {
    return -1;
  }
  if(offset < 0){
    uint64 magnitude = (uint64)(-(offset + 1)) + 1;
    if(magnitude > base) return -1;
    result = base - magnitude;
  } else {
    if(base + (uint64)offset < base) return -1;
    result = base + (uint64)offset;
  }
  if(result > 0x7fffffffffffffffULL)
    return -1;
  f->off = result;
  return result;
}

uint64
sys_socket_listen(void)
{
  int port, backlog, handle, fd;
  struct file *f;
  if(argint(0, &port) < 0 || argint(1, &backlog) < 0 ||
     (handle = net_tcp_listen(port, backlog)) < 0)
    return -1;
  if((f = filealloc()) == 0){
    net_tcp_close(handle);
    return -1;
  }
  f->type = FD_SOCKET;
  f->readable = 1;
  f->writable = 1;
  f->socket = handle;
  if((fd = fdalloc(f)) < 0){
    fileclose(f);
    return -1;
  }
  return fd;
}

uint64
sys_socket_accept(void)
{
  struct file *listener, *child;
  int child_handle, child_fd;
  if(argfd(0, 0, &listener) < 0 || listener->type != FD_SOCKET ||
     listener->socket < 0)
    return -1;
  if((child_handle = net_tcp_accept(listener->socket,
          (listener->flags & O_NONBLOCK) != 0)) < 0)
    return -1;
  if((child = filealloc()) == 0){
    net_tcp_close(child_handle);
    return -1;
  }
  child->type = FD_SOCKET;
  child->readable = 1;
  child->writable = 1;
  child->socket = child_handle;
  if((child_fd = fdalloc(child)) < 0){
    fileclose(child);
    return -1;
  }
  return child_fd;
}

uint64
sys_fcntl(void)
{
  struct file *f;
  int cmd, value;
  if(argfd(0, 0, &f) < 0 || argint(1, &cmd) < 0 || argint(2, &value) < 0)
    return -1;
  if(cmd == F_GETFL)
    return f->flags;
  if(cmd == F_SETFL){
    f->flags = (f->flags & ~O_NONBLOCK) | (value & O_NONBLOCK);
    return 0;
  }
  return -1;
}

uint64
sys_epoll_create(void)
{
  struct epoll *ep;
  struct file *f;
  int fd;
  if((ep = epollalloc()) == 0)
    return -1;
  if((f = filealloc()) == 0){
    epollclose(ep);
    return -1;
  }
  f->type = FD_EPOLL;
  f->readable = f->writable = 0;
  f->epoll = ep;
  if((fd = fdalloc(f)) < 0){
    fileclose(f);
    return -1;
  }
  return fd;
}

uint64
sys_epoll_ctl(void)
{
  struct file *epfile, *target;
  struct epoll_event event, *eventp = 0;
  int op, fd;
  uint64 address;
  if(argfd(0, 0, &epfile) < 0 || epfile->type != FD_EPOLL ||
     argint(1, &op) < 0 || argint(2, &fd) < 0 ||
     argaddr(3, &address) < 0)
    return -1;
  if(op != EPOLL_CTL_DEL){
    if(copyin(myproc()->vm->pagetable, (char*)&event, address,
              sizeof(event)) < 0)
      return -1;
    eventp = &event;
  }
  if(fd < 0 || fd >= NOFILE || (target = myproc()->ofile[fd]) == 0 ||
     target == epfile)
    return -1;
  return epollctl(epfile->epoll, op, fd, target, eventp);
}

uint64
sys_epoll_wait(void)
{
  struct file *epfile;
  uint64 events;
  int maxevents, timeout;
  if(argfd(0, 0, &epfile) < 0 || epfile->type != FD_EPOLL ||
     argaddr(1, &events) < 0 || argint(2, &maxevents) < 0 ||
     argint(3, &timeout) < 0)
    return -1;
  return epollwait(epfile->epoll, events, maxevents, timeout);
}

uint64
sys_dup(void)
{
  struct file *f;
  int fd;

  if(argfd(0, 0, &f) < 0)
    return -1;
  if((fd=fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

uint64
sys_read(void)
{
  struct file *f;
  int n;
  uint64 p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argaddr(1, &p) < 0)
    return -1;
  return fileread(f, p, n);
}

// Minimal device mmap.  Filesystems do not yet provide page-cache-backed
// mappings; a character driver owns the selected virtual address and physical
// pages.  This is sufficient for coherent DMA mappings such as /dev/video0.
uint64
sys_mmap(void)
{
  struct file *f;
  uint64 addr, len, off;
  int prot, flags;

  if(argaddr(0, &addr) < 0 || argaddr(1, &len) < 0 ||
     argint(2, &prot) < 0 || argint(3, &flags) < 0 ||
     argfd(4, 0, &f) < 0 || argaddr(5, &off) < 0 ||
     f->type != FD_DEVICE)
    return (uint64)-1;
  return chrdev_mmap(f, addr, len, prot, flags, off);
}

uint64
sys_munmap(void)
{
  struct file *f;
  uint64 addr, len;

  // The mapping callback is selected by the mapping address.  For now only
  // /dev/video0 supplies mmap, so find an open camera descriptor belonging to
  // this process rather than adding a global VMA table prematurely.
  if(argaddr(0, &addr) < 0 || argaddr(1, &len) < 0)
    return -1;
  for(int fd = 0; fd < NOFILE; fd++){
    f = myproc()->ofile[fd];
    if(f && f->type == FD_DEVICE && f->major == CAMERA)
      return chrdev_munmap(f, addr, len);
  }
  return camera_munmap_current(addr, len);
}

uint64
sys_write(void)
{
  struct file *f;
  int n;
  uint64 p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argaddr(1, &p) < 0)
    return -1;

  return filewrite(f, p, n);
}

uint64
sys_close(void)
{
  int fd;
  struct file *f;

  if(argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

uint64
sys_fstat(void)
{
  struct file *f;
  uint64 st; // user pointer to struct stat

  if(argfd(0, 0, &f) < 0 || argaddr(1, &st) < 0)
    return -1;
  return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
uint64
sys_link(void)
{
  char name[NAMEMAX], new[MAXPATH], old[MAXPATH];
  struct inode *dp, *ip;

  if(argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0 ||
     abspath(old) < 0 || abspath(new) < 0)
    return -1;

  begin_op();
  if((ip = namei(old)) == 0){
    end_op();
    return -1;
  }
  if((dp = nameiparent(new, name)) == 0){
    iput(ip);
    end_op();
    return -1;
  }
  int r = fs_link(dp, name, ip);
  iput(dp);
  iput(ip);
  end_op();
  return r;
}

uint64
sys_unlink(void)
{
  struct inode *ip, *dp;
  char name[NAMEMAX], path[MAXPATH];
  int r = -1;

  if(argstr(0, path, MAXPATH) < 0 || abspath(path) < 0)
    return -1;

  begin_op();
  if((dp = nameiparent(path, name)) == 0){
    end_op();
    return -1;
  }
  ilock(dp);
  ip = dp->type == T_DIR ? dp->iop->lookup(dp, name) : 0;
  iunlock(dp);
  if(ip){
    int busy = fs_is_mountpoint(ip);    // EBUSY: something is mounted on it
    iput(ip);
    if(!busy)
      r = fs_unlink(dp, name);
  }
  iput(dp);
  end_op();
  return r;
}

static struct inode*
create(char *path, short type, short major, short minor)
{
  struct inode *dp;
  char name[NAMEMAX];

  if((dp = nameiparent(path, name)) == 0)
    return 0;
  return fs_create(dp, name, type, major, minor);
}

uint64
sys_open(void)
{
  char path[MAXPATH];
  int fd, omode;
  struct file *f;
  struct inode *ip;
  int n;

  if((n = argstr(0, path, MAXPATH)) < 0 || argint(1, &omode) < 0 ||
     abspath(path) < 0)
    return -1;

  begin_op();

  if(omode & O_CREATE){
    ip = create(path, T_FILE, 0, 0);
    if(ip == 0){
      end_op();
      return -1;
    }
  } else {
    if((ip = namei(path)) == 0){
      end_op();
      return -1;
    }
    ilock(ip);
    if(ip->type == T_DIR && omode != O_RDONLY){
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  if(ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)){
    iunlockput(ip);
    end_op();
    return -1;
  }

  // Filesystem side of open: read-only mounts, O_TRUNC, and the
  // filesystem's own open hook (FAT32 needs a write handle).
  if(ip->type == T_FILE){
    int wr = (omode & (O_WRONLY | O_RDWR)) != 0;
    if(((wr || (omode & O_TRUNC)) && fs_is_readonly(ip)) ||
       ((omode & O_TRUNC) && ip->iop->truncate(ip, 0) < 0) ||
       (ip->iop->open && ip->iop->open(ip, omode) < 0)){
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if(f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }

  if(ip->type == T_DEVICE){
    f->type = FD_DEVICE;
    f->major = ip->major;
  } else {
    f->type = FD_INODE;
    f->off = 0;
  }
  f->ip = ip;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);
  f->flags = omode & (O_WRONLY | O_RDWR | O_NONBLOCK);

  // Give the driver a chance to attach per-open state.  On failure the
  // driver's release() must not run, so detach f as a plain FD_NONE file.
  if(f->type == FD_DEVICE && chrdev_open(f) < 0){
    myproc()->ofile[fd] = 0;
    f->type = FD_NONE;
    fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }

  iunlock(ip);
  end_op();

  return fd;
}

uint64
sys_mkdir(void)
{
  char path[MAXPATH];
  struct inode *ip;

  if(argstr(0, path, MAXPATH) < 0 || abspath(path) < 0)
    return -1;
  begin_op();
  if((ip = create(path, T_DIR, 0, 0)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_mknod(void)
{
  struct inode *ip;
  char path[MAXPATH];
  int major, minor;

  if(argstr(0, path, MAXPATH) < 0 || abspath(path) < 0)
    return -1;
  begin_op();
  if(argint(1, &major) < 0 ||
     argint(2, &minor) < 0 ||
     (ip = create(path, T_DEVICE, major, minor)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_chdir(void)
{
  char path[MAXPATH];
  struct inode *ip;
  struct proc *p = myproc();

  if(argstr(0, path, MAXPATH) < 0 || abspath(path) < 0)
    return -1;
  begin_op();
  if((ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  if(ip->type != T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  iput(p->cwd);
  end_op();
  p->cwd = ip;
  safestrcpy(p->cwdpath, path, sizeof(p->cwdpath));
  return 0;
}

uint64
sys_exec(void)
{
  char path[MAXPATH], *argv[MAXARG];
  int i;
  uint64 uargv, uarg;

  if(argstr(0, path, MAXPATH) < 0 || argaddr(1, &uargv) < 0 ||
     abspath(path) < 0){
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for(i=0;; i++){
    if(i >= NELEM(argv)){
      goto bad;
    }
    if(fetchaddr(uargv+sizeof(uint64)*i, (uint64*)&uarg) < 0){
      goto bad;
    }
    if(uarg == 0){
      argv[i] = 0;
      break;
    }
    argv[i] = kalloc();
    if(argv[i] == 0)
      goto bad;
    if(fetchstr(uarg, argv[i], PGSIZE) < 0)
      goto bad;
  }

  int ret = exec(path, argv);

  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);

  return ret;

 bad:
  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);
  return -1;
}

uint64
sys_pipe(void)
{
  uint64 fdarray; // user pointer to array of two integers
  struct file *rf, *wf;
  int fd0, fd1;
  struct proc *p = myproc();

  if(argaddr(0, &fdarray) < 0)
    return -1;
  if(pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){
    if(fd0 >= 0)
      p->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  if(copyout(p->vm->pagetable, fdarray, (char*)&fd0, sizeof(fd0)) < 0 ||
     copyout(p->vm->pagetable, fdarray+sizeof(fd0), (char *)&fd1, sizeof(fd1)) < 0){
    p->ofile[fd0] = 0;
    p->ofile[fd1] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  return 0;
}

// Create a pseudoterminal pair. fdarray[0] is the master used by sshd;
// fdarray[1] is the slave attached to the child shell's fd 0/1/2.
uint64
sys_pty_open(void)
{
  uint64 fdarray;
  struct file *master, *slave;
  int fdm = -1, fds = -1;
  struct proc *p = myproc();

  if(argaddr(0, &fdarray) < 0 || ptyalloc(&master, &slave) < 0)
    return -1;
  if((fdm = fdalloc(master)) < 0 || (fds = fdalloc(slave)) < 0)
    goto bad;
  if(copyout(p->vm->pagetable, fdarray, (char *)&fdm, sizeof(fdm)) < 0 ||
     copyout(p->vm->pagetable, fdarray + sizeof(fdm), (char *)&fds,
             sizeof(fds)) < 0)
    goto bad;
  return 0;

bad:
  if(fdm >= 0)
    p->ofile[fdm] = 0;
  if(fds >= 0)
    p->ofile[fds] = 0;
  fileclose(master);
  fileclose(slave);
  return -1;
}
