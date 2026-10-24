//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include <kernel/types.h>
#include "aarch64.h"
#include <kernel/defs.h>
#include <kernel/param.h>
#include <kernel/stat.h>
#include <kernel/spinlock.h>
#include <kernel/proc.h>
#include <kernel/fs.h>
#include <kernel/sleeplock.h>
#include <kernel/file.h>
#include <kernel/fcntl.h>
#include <kernel/device.h>
#include <kernel/ext2.h>
#include <kernel/epoll.h>
#include <kernel/vfs.h>
#include <kernel/socket.h>
#include <kernel/fs_trace.h>
#include <uapi/poll.h>

// Keep open tracing useful on a serial console without turning a long-running
// camera/server workload into an unbounded log stream.
#if VFS_TRACE
static int vfs_open_trace_budget = 0x7fffffff;

static int
vfs_open_trace_take(void)
{
  // Diagnostic only: a race can print one extra line but cannot affect VFS
  // state.  Avoid libgcc atomic helpers in this freestanding kernel.
  if(vfs_open_trace_budget <= 0)
    return 0;
  vfs_open_trace_budget--;
  return 1;
}
#endif

static uint16
net16(uint16 value)
{
  return (value << 8) | (value >> 8);
}

static int
socket_fread(struct file *f, int user_dst, uint64 addr, int n)
{
  if(!user_dst)
    return -1;
  return net_tcp_read(f->socket, addr, n,
                      (f->flags & O_NONBLOCK) != 0);
}

static int
socket_fwrite(struct file *f, int user_src, uint64 addr, int n)
{
  if(!user_src)
    return -1;
  return net_tcp_write(f->socket, addr, n,
                       (f->flags & O_NONBLOCK) != 0);
}

static void
socket_release(struct file *f)
{
  if(f->socket >= 0)
    net_tcp_close(f->socket);
}

static int
socket_poll(struct file *f, int events)
{
  return f->socket >= 0 ? net_tcp_poll(f->socket, events) : 0;
}

static const struct file_operations socket_file_ops = {
  .fread = socket_fread,
  .fwrite = socket_fwrite,
  .release = socket_release,
  .poll = socket_poll,
};

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
  VFSTRACE("sys_mount: source=%s target=%s type=%s flags=%x\n",
           source, target, fstype, flags);
  int r = vfsmount(source, target, fstype, flags);
  VFSTRACE("sys_mount: target=%s result=%d\n", target, r);
  return r;
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
  ireadlock(odp);
  t = odp->type == T_DIR ? odp->iop->lookup(odp, oname) : 0;
  ireadunlock(odp);
  if(t){
    int busy = fs_is_mountpoint(t);   // EBUSY: something is mounted on it
    iput(t);
    if(!busy)
      r = fs_rename(odp, oname, ndp, nname);
  }
  iput(ndp);
  iput(odp);
  end_op();
  VFSTRACE("sys_rename: old=%s new=%s result=%d\n", oldpath, newpath, r);
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

// readlink(2): resolve the target of a symbolic link.  Only sysfs has links in
// this kernel, so the VFS readlink op is present just there; other filesystems
// leave the hook NULL and the call fails, as Linux returns EINVAL for a non-
// link.  The bytes copied out are not NUL-terminated (POSIX); we return the
// length so the caller sizes its own buffer.
uint64
sys_readlink(void)
{
  char path[MAXPATH], target[MAXPATH];
  uint64 address;
  int size, len;
  struct inode *ip;

  if(argstr(0, path, MAXPATH) < 0 || argaddr(1, &address) < 0 ||
     argint(2, &size) < 0 || size <= 0)
    return -1;
  if((ip = namei(path)) == 0)
    return -1;
  if(ip->iop->readlink == 0 ||
     (len = ip->iop->readlink(ip, target, sizeof(target))) < 0){
    iput(ip);
    return -1;
  }
  iput(ip);
  if(len > size)
    len = size;                        // truncate to the caller's buffer
  if(copyout(myproc()->vm->pagetable, address, target, len) < 0)
    return -1;
  return len;
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
  f->f_op = &socket_file_ops;
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
  f->f_op = &socket_file_ops;
  f->socket_backlog = backlog;
  return 0;
}

// connect(fd, &sockaddr_in, len): active TCP open on an unbound socket.
uint64
sys_connect(void)
{
  struct file *f;
  struct sockaddr_in address;
  uint64 user_address;
  int length, handle;
  uint32 ip;

  if(argfd(0, 0, &f) < 0 || f->type != FD_SOCKET || f->socket >= 0 ||
     argaddr(1, &user_address) < 0 || argint(2, &length) < 0 ||
     length < sizeof(address) ||
     copyin(myproc()->vm->pagetable, (char *)&address, user_address,
            sizeof(address)) < 0 || address.sin_family != AF_INET)
    return -1;
  // sin_addr/sin_port are in network byte order; the stack uses host order.
  ip = ((address.sin_addr.s_addr & 0xff) << 24) |
       ((address.sin_addr.s_addr & 0xff00) << 8) |
       ((address.sin_addr.s_addr >> 8) & 0xff00) |
       (address.sin_addr.s_addr >> 24);
  if((handle = net_tcp_connect(ip, net16(address.sin_port))) < 0)
    return -1;
  f->socket = handle;
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
  long answer = -1;

  if(argfd(0, 0, &f) < 0 || argaddr(1, &raw_offset) < 0 ||
     argint(2, &whence) < 0)
    return -1;
  offset = (long)raw_offset;
  int blk = f->type == FD_DEVICE && f->major == BLOCKDEV;
  if(f->type != FD_INODE && !blk)
    return -1;
  mutex_lock(&f->pos_lock);
  if(whence == 0){
    base = 0;
  } else if(whence == 1){
    base = f->off;
  } else if(whence == 2){
    if(blk){
      if((base = blkdev_size(f->ip->minor)) == 0)
        goto out;
    } else {
      ireadlock(f->ip);
      base = f->ip->size;
      ireadunlock(f->ip);
    }
  } else {
    goto out;
  }
  if(offset < 0){
    uint64 magnitude = (uint64)(-(offset + 1)) + 1;
    if(magnitude > base) goto out;
    result = base - magnitude;
  } else {
    if(base + (uint64)offset < base) goto out;
    result = base + (uint64)offset;
  }
  if(result > 0x7fffffffffffffffULL)
    goto out;
  f->off = result;
  answer = result;
out:
  mutex_unlock(&f->pos_lock);
  return answer;
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
  f->f_op = &socket_file_ops;
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
  child->f_op = &socket_file_ops;
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
  epollfile_setup(f, ep);
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
sys_ioctl(void)
{
  struct file *f;
  uint64 cmd, arg;
  if(argfd(0, 0, &f) < 0 || argaddr(1, &cmd) < 0 ||
     argaddr(2, &arg) < 0)
    return -1;
  return fileioctl(f, cmd, arg);
}

uint64
sys_poll(void)
{
  struct pollfd pfds[NOFILE];
  struct file *files[NOFILE];
  uint64 address;
  int nfds, timeout_ms;
  uint start, timeout_ticks;

  if(argaddr(0, &address) < 0 || argint(1, &nfds) < 0 ||
     argint(2, &timeout_ms) < 0 || nfds < 0 || nfds > NOFILE ||
     timeout_ms < -1)
    return -1;
  if(nfds && copyin(myproc()->vm->pagetable, (char *)pfds, address,
                    nfds * sizeof(pfds[0])) < 0)
    return -1;
  acquire(&tickslock);
  start = ticks;
  release(&tickslock);
  timeout_ticks = timeout_ms < 0 ? 0 : (timeout_ms + 99) / 100;

  for(;;){
    int ready = 0;
    memset(files, 0, sizeof(files));
    for(int i = 0; i < nfds; i++){
      pfds[i].revents = 0;
      if(pfds[i].fd < 0)
        continue;
      if(pfds[i].fd >= NOFILE || myproc()->ofile[pfds[i].fd] == 0){
        pfds[i].revents = POLLNVAL;
        ready++;
        continue;
      }
      files[i] = filedup(myproc()->ofile[pfds[i].fd]);
      pfds[i].revents = filepoll(files[i], pfds[i].events);
      fileclose(files[i]);
      if(pfds[i].revents)
        ready++;
    }
    if(ready || timeout_ms == 0){
      if(nfds && copyout(myproc()->vm->pagetable, address, (char *)pfds,
                         nfds * sizeof(pfds[0])) < 0)
        return -1;
      return ready;
    }
    if(myproc()->killed)
      return -1;
    acquire(&tickslock);
    if(timeout_ms >= 0 && ticks - start >= timeout_ticks){
      release(&tickslock);
      if(nfds && copyout(myproc()->vm->pagetable, address, (char *)pfds,
                         nfds * sizeof(pfds[0])) < 0)
        return -1;
      return 0;
    }
    sleep(&ticks, &tickslock);
    release(&tickslock);
  }
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
sys_dup2(void)
{
  struct file *f, *old;
  struct proc *p = myproc();
  int oldfd, newfd;

  if(argfd(0, &oldfd, &f) < 0 || argint(1, &newfd) < 0 ||
     newfd < 0 || newfd >= NOFILE)
    return -1;
  if(oldfd == newfd)
    return newfd;
  old = p->ofile[newfd];
  p->ofile[newfd] = filedup(f);
  if(old)
    fileclose(old);
  return newfd;
}

uint64
sys_getcwd(void)
{
  struct proc *p = myproc();
  uint64 address;
  int size, length;

  if(argaddr(0, &address) < 0 || argint(1, &size) < 0 || size <= 0)
    return -1;
  length = strlen(p->cwdpath) + 1;
  if(length > size || copyout(p->vm->pagetable, address, p->cwdpath,
                              length) < 0)
    return -1;
  return address;
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
     argfd(4, 0, &f) < 0 || argaddr(5, &off) < 0)
    return (uint64)-1;
  return filemmap(f, addr, len, prot, flags, off);
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
    if(f && f->f_op && f->f_op->munmap)
      return filemunmap(f, addr, len);
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
  ireadlock(dp);
  ip = dp->type == T_DIR ? dp->iop->lookup(dp, name) : 0;
  ireadunlock(dp);
  if(ip){
    int busy = fs_is_mountpoint(ip);    // EBUSY: something is mounted on it
    iput(ip);
    if(!busy)
      r = fs_unlink(dp, name);
  }
  iput(dp);
  end_op();
  VFSTRACE("sys_unlink: path=%s result=%d\n", path, r);
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
    file_inode_setup(f);
  }
  f->ip = ip;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);
  f->flags = omode & (O_WRONLY | O_RDWR | O_NONBLOCK);

#if VFS_TRACE
  if(vfs_open_trace_take()){
    struct super_block *sb = getsuper(ip->dev);
    printf("vfs-trace: sys_open: path=%s -> fd=%d fs=%s dev=%d ino=%d type=%d mode=%x dispatch=%s\n",
           path, fd, sb ? sb->type : "unknown", ip->dev, ip->inum,
           ip->type, omode, ip->type == T_DEVICE ? "file_operations" :
           "inode_ops(read/write)");
  }
#endif

  // Give the driver a chance to attach per-open state.  On failure the
  // driver's release() must not run, so detach f as a plain FD_NONE file.
  if(f->type == FD_DEVICE && chrdev_open(f) < 0){
    myproc()->ofile[fd] = 0;
    f->type = FD_NONE;
    f->f_op = 0;
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
  VFSTRACE("sys_mkdir: path=%s result=0\n", path);
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
  ireadlock(ip);
  if(ip->type != T_DIR){
    ireadunlockput(ip);
    end_op();
    return -1;
  }
  ireadunlock(ip);
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
