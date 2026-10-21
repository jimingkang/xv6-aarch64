#include <uapi/types.h>
#include <uapi/stat.h>
#include <uapi/syscall.h>
#include "user/user.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <uapi/poll.h>

// The only user-space entry that executes svc.  Public functions below form
// the libc ABI and deliberately hide the kernel syscall-number ABI.
extern long __syscall(long, long, long, long, long, long, long);

static long
sc(long number, long a0, long a1, long a2, long a3, long a4, long a5)
{
  long result = __syscall(number, a0, a1, a2, a3, a4, a5);
  // The current kernel returns only -1 rather than Linux-style -errno.  Keep
  // errno useful until individual kernel paths return precise error numbers.
  if(result < 0)
    errno = EIO;
  return result;
}

#define P(v) ((long)(uint64)(v))
#define I(v) ((long)(v))
#define SC0(n) sc(SYS_##n, 0, 0, 0, 0, 0, 0)
#define SC1(n,a) sc(SYS_##n, (a), 0, 0, 0, 0, 0)
#define SC2(n,a,b) sc(SYS_##n, (a), (b), 0, 0, 0, 0)
#define SC3(n,a,b,c) sc(SYS_##n, (a), (b), (c), 0, 0, 0)
#define SC4(n,a,b,c,d) sc(SYS_##n, (a), (b), (c), (d), 0, 0)
#define SC5(n,a,b,c,d,e) sc(SYS_##n, (a), (b), (c), (d), (e), 0)
#define SC6(n,a,b,c,d,e,f) sc(SYS_##n, (a), (b), (c), (d), (e), (f))

int fork(void) { return SC0(fork); }
int wait(int *status) { return SC1(wait, P(status)); }
int waitpid(int pid, int *status, int options)
{ return SC3(waitpid, I(pid), P(status), I(options)); }
int pipe(int *fds) { return SC1(pipe, P(fds)); }
int read(int fd, void *buf, int count)
{ return SC3(read, I(fd), P(buf), I(count)); }
int write(int fd, const void *buf, int count)
{ return SC3(write, I(fd), P(buf), I(count)); }
int close(int fd) { return SC1(close, I(fd)); }
int kill(int pid) { return SC1(kill, I(pid)); }
int exec(char *path, char **argv) { return SC2(exec, P(path), P(argv)); }
int open(const char *path, int flags, ...)
{
  // The current filesystems do not implement permission bits, so the optional
  // POSIX mode argument used with O_CREAT is intentionally ignored.
  return SC2(open, P(path), I(flags));
}
int mknod(const char *path, int mode, unsigned long device)
{
  if((mode & 7) != T_DEVICE){
    errno = EINVAL;
    return -1;
  }
  return SC3(mknod, P(path), I((device >> 16) & 0xffff),
             I(device & 0xffff));
}
int unlink(const char *path) { return SC1(unlink, P(path)); }
int fstat(int fd, struct stat *st) { return SC2(fstat, I(fd), P(st)); }
int link(const char *oldpath, const char *newpath)
{ return SC2(link, P(oldpath), P(newpath)); }
int mkdir(const char *path) { return SC1(mkdir, P(path)); }
int chdir(const char *path) { return SC1(chdir, P(path)); }
int dup(int fd) { return SC1(dup, I(fd)); }
int dup2(int oldfd, int newfd) { return SC2(dup2, I(oldfd), I(newfd)); }
int getpid(void) { return SC0(getpid); }
int getppid(void) { return SC0(getppid); }
int gettid(void) { return SC0(gettid); }
char *getcwd(char *buf, int size) { return (char *)SC2(getcwd, P(buf), I(size)); }
int readlink(const char *path, char *buf, int sz) { return SC3(readlink, P(path), P(buf), I(sz)); }
int reboot(int cmd) { return SC1(reboot, I(cmd)); }
int connect(int fd, const void *address, int length)
{ return SC3(connect, I(fd), P(address), I(length)); }
uint32 net_dns(void) { return (uint32)SC0(net_dns); }
char *sbrk(int increment) { return (char *)SC1(sbrk, I(increment)); }
int sleep_ticks(int ticks) { return SC1(sleep, I(ticks)); }
int uptime(void) { return SC0(uptime); }
int ps(void *buf, int size) { return SC2(ps, P(buf), I(size)); }
int ext2read(const char *path, void *buf, int size, uint64 offset)
{ return SC4(ext2read, P(path), P(buf), I(size), I(offset)); }
int ext2readdir(const char *path, int index, struct ext2_user_dirent *entry)
{ return SC3(ext2readdir, P(path), I(index), P(entry)); }
int sync_create(int type, int value) { return SC2(sync_create, I(type), I(value)); }
int sync_wait(int handle) { return SC1(sync_wait, I(handle)); }
int sync_signal(int handle, int count) { return SC2(sync_signal, I(handle), I(count)); }
int sync_reset(int handle) { return SC1(sync_reset, I(handle)); }
int sync_atomic(int handle, int op, int arg, int arg2)
{ return SC4(sync_atomic, I(handle), I(op), I(arg), I(arg2)); }
int sync_destroy(int handle) { return SC1(sync_destroy, I(handle)); }
int udp_bind(int port) { return SC1(udp_bind, I(port)); }
int udp_unbind(int port) { return SC1(udp_unbind, I(port)); }
int udp_send(uint32 ip, int sport, int dport, const void *buf, int size)
{ return SC5(udp_send, I(ip), I(sport), I(dport), P(buf), I(size)); }
int udp_recv(int port, uint32 *ip, uint16 *sport, void *buf, int size)
{ return SC5(udp_recv, I(port), P(ip), P(sport), P(buf), I(size)); }
int udp_tryrecv(int port, uint32 *ip, uint16 *sport, void *buf, int size)
{ return SC5(udp_tryrecv, I(port), P(ip), P(sport), P(buf), I(size)); }
int udp_recv_timeout(int port, uint32 *ip, uint16 *sport, void *buf,
                     int size, int ticks)
{ return SC6(udp_recv_timeout, I(port), P(ip), P(sport), P(buf), I(size), I(ticks)); }
int icmp_send(uint32 ip, int id, int seq, const void *buf, int size)
{ return SC5(icmp_send, I(ip), I(id), I(seq), P(buf), I(size)); }
int icmp_recv(int id, uint32 *ip, uint16 *seq, void *buf, int size)
{ return SC5(icmp_recv, I(id), P(ip), P(seq), P(buf), I(size)); }
int mount(const char *src, const char *target, const char *type, int flags)
{ return SC4(mount, P(src), P(target), P(type), I(flags)); }
int tty_attach(int fd) { return SC1(tty_attach, I(fd)); }
int tty_set_foreground(int pgid) { return SC1(tty_set_foreground, I(pgid)); }
int vmdump(int pid) { return SC1(vmdump, I(pid)); }
int wifi_connect(const char *ssid, const char *password)
{ return SC2(wifi_connect, P(ssid), P(password)); }
int net_dhcp(const char *name) { return SC1(net_dhcp, P(name)); }
int tcp_listen(int port) { return SC1(tcp_listen, I(port)); }
int tcp_accept(int handle) { return SC1(tcp_accept, I(handle)); }
int tcp_read(int handle, void *buf, int size)
{ return SC3(tcp_read, I(handle), P(buf), I(size)); }
int tcp_write(int handle, const void *buf, int size)
{ return SC3(tcp_write, I(handle), P(buf), I(size)); }
int tcp_close(int handle) { return SC1(tcp_close, I(handle)); }
int socket_listen(int port, int backlog)
{ return SC2(socket_listen, I(port), I(backlog)); }
int socket_accept(int fd) { return SC1(socket_accept, I(fd)); }
int epoll_create(void) { return SC0(epoll_create); }
int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event)
{ return SC4(epoll_ctl, I(epfd), I(op), I(fd), P(event)); }
int epoll_wait(int epfd, struct epoll_event *events, int count, int timeout)
{ return SC4(epoll_wait, I(epfd), P(events), I(count), I(timeout)); }
int fcntl(int fd, int command, ...)
{
  int argument = 0;
  va_list arguments;

  // F_GETFL has no third argument.  F_SETFL consumes the integer flags.
  if(command != F_GETFL){
    va_start(arguments, command);
    argument = va_arg(arguments, int);
    va_end(arguments);
  }
  return SC3(fcntl, I(fd), I(command), I(argument));
}
int pty_open(int *fds) { return SC1(pty_open, P(fds)); }
int getrandom(void *buf, int size) { return SC2(getrandom, P(buf), I(size)); }
int rename(const char *oldpath, const char *newpath)
{ return SC2(rename, P(oldpath), P(newpath)); }
int sched_setscheduler(int pid, int policy, int priority)
{ return SC3(sched_setscheduler, I(pid), I(policy), I(priority)); }
int sched_getinfo(int pid, struct sched_info *info)
{ return SC2(sched_getinfo, I(pid), P(info)); }
int setnice(int pid, int nice) { return SC2(setnice, I(pid), I(nice)); }
int sched_setaffinity(int pid, uint mask)
{ return SC2(sched_setaffinity, I(pid), I(mask)); }
int sched_yield(void) { return SC0(sched_yield); }
uint64 clock_us(void) { return (uint64)SC0(clock_us); }
int getcpu(void) { return SC0(getcpu); }
int clone(void (*entry)(void *), void *arg, void *stack)
{ return SC3(clone, P(entry), P(arg), P(stack)); }
int tjoin(int tid, int *status) { return SC2(tjoin, I(tid), P(status)); }
int socket(int domain, int type, int protocol)
{ return SC3(socket, I(domain), I(type), I(protocol)); }
int bind(int fd, const void *address, int length)
{ return SC3(bind, I(fd), P(address), I(length)); }
int listen(int fd, int backlog) { return SC2(listen, I(fd), I(backlog)); }
long lseek(int fd, long offset, int whence)
{ return SC3(lseek, I(fd), I(offset), I(whence)); }
int fsync(int fd) { return SC1(fsync, I(fd)); }
int fdatasync(int fd) { return SC1(fdatasync, I(fd)); }
int ftruncate(int fd, uint64 length)
{ return SC2(ftruncate, I(fd), I(length)); }
int pread(int fd, void *buf, int size, uint64 offset)
{ return SC4(pread, I(fd), P(buf), I(size), I(offset)); }
int pwrite(int fd, const void *buf, int size, uint64 offset)
{ return SC4(pwrite, I(fd), P(buf), I(size), I(offset)); }
void *mmap(void *addr, unsigned long len, int prot, int flags, int fd,
           long offset)
{ return (void *)SC6(mmap, P(addr), I(len), I(prot), I(flags), I(fd), I(offset)); }
int munmap(void *addr, unsigned long len) { return SC2(munmap, P(addr), I(len)); }
int init_module(void *image, unsigned long size, const char *args)
{ return SC3(init_module, P(image), I(size), P(args)); }
int delete_module(const char *name) { return SC1(delete_module, P(name)); }
int poll(struct pollfd *fds, unsigned long nfds, int timeout)
{ return SC3(poll, P(fds), I(nfds), I(timeout)); }
int ioctl(int fd, unsigned long request, ...)
{
  unsigned long argument = 0;
  va_list arguments;
  va_start(arguments, request);
  argument = va_arg(arguments, unsigned long);
  va_end(arguments);
  return SC3(ioctl, I(fd), I(request), I(argument));
}

int
exit(int status)
{
  SC1(exit, I(status));
  for(;;)
    ;
}

int
texit(int status)
{
  SC1(texit, I(status));
  for(;;)
    ;
}
