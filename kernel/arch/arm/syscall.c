#include "kernel/types.h"
#include "kernel/param.h"
#include "kernel/memlayout.h"
#include "kernel/aarch64.h"
#include "kernel/spinlock.h"
#include "kernel/proc.h"
#include "kernel/syscall.h"
#include "kernel/defs.h"

// Fetch the uint64 at addr from the current process.
int
fetchaddr(uint64 addr, uint64 *ip)
{
  struct proc *p = myproc();
  if(addr >= p->vm->sz || sizeof(uint32) > p->vm->sz-addr)
    return -1;
  *ip = 0;
  if(copyin(p->vm->pagetable, (char *)ip, addr, sizeof(uint32)) != 0)
    return -1;
  return 0;
}

// Fetch the nul-terminated string at addr from the current process.
// Returns length of string, not including nul, or -1 for error.
int
fetchstr(uint64 addr, char *buf, int max)
{
  struct proc *p = myproc();
  int err = copyinstr(p->vm->pagetable, buf, addr, max);
  if(err < 0)
    return err;
  return strlen(buf);
}

static uint64
argraw(int n)
{
  struct proc *p = myproc();
  switch (n) {
  case 0:
    return p->trapframe->x0;
  case 1:
    return p->trapframe->x1;
  case 2:
    return p->trapframe->x2;
  case 3:
    return p->trapframe->x3;
  case 4:
    return p->trapframe->x4;
  case 5:
    return p->trapframe->x5;
  }
  panic("argraw");
  return -1;
}

// Fetch the nth 32-bit system call argument.
int
argint(int n, int *ip)
{
  *ip = argraw(n);
  return 0;
}

// Retrieve an argument as a pointer.
// Doesn't check for legality, since
// copyin/copyout will do that.
int
argaddr(int n, uint64 *ip)
{
  *ip = argraw(n);
  return 0;
}

// Fetch the nth word-sized system call argument as a null-terminated string.
// Copies into buf, at most max.
// Returns string length if OK (including nul), -1 if error.
int
argstr(int n, char *buf, int max)
{
  uint64 addr;
  if(argaddr(n, &addr) < 0)
    return -1;
  return fetchstr(addr, buf, max);
}


extern uint64 sys_fork(void);
extern uint64 sys_exit(void);
extern uint64 sys_wait(void);
extern uint64 sys_pipe(void);
extern uint64 sys_read(void);
extern uint64 sys_kill(void);
extern uint64 sys_exec(void);
extern uint64 sys_fstat(void);
extern uint64 sys_chdir(void);
extern uint64 sys_dup(void);
extern uint64 sys_getpid(void);
extern uint64 sys_sbrk(void);
extern uint64 sys_sleep(void);
extern uint64 sys_uptime(void);
extern uint64 sys_open(void);
extern uint64 sys_write(void);
extern uint64 sys_mknod(void);
extern uint64 sys_unlink(void);
extern uint64 sys_link(void);
extern uint64 sys_mkdir(void);
extern uint64 sys_close(void);
extern uint64 sys_ps(void);
extern uint64 sys_tty_attach(void);
extern uint64 sys_tty_set_foreground(void);
extern uint64 sys_sched_yield(void);
extern uint64 sys_vmdump(void);
extern uint64 sys_mmap(void);
extern uint64 sys_munmap(void);
extern uint64 sys_ioctl(void);
extern uint64 sys_mount(void);
extern uint64 sys_rename(void);
extern uint64 sys_udp_bind(void);
extern uint64 sys_udp_unbind(void);
extern uint64 sys_udp_send(void);
extern uint64 sys_udp_recv(void);
extern uint64 sys_udp_tryrecv(void);
extern uint64 sys_udp_recv_timeout(void);
extern uint64 sys_icmp_send(void);
extern uint64 sys_icmp_recv(void);
extern uint64 sys_wifi_connect(void);
extern uint64 sys_net_dhcp(void);
static uint64 (*calls[])(void) = {
  [SYS_fork] = sys_fork,
  [SYS_exit] = sys_exit,
  [SYS_wait] = sys_wait,
  [SYS_pipe] = sys_pipe,
  [SYS_read] = sys_read,
  [SYS_kill] = sys_kill,
  [SYS_exec] = sys_exec,
  [SYS_fstat] = sys_fstat,
  [SYS_chdir] = sys_chdir,
  [SYS_dup] = sys_dup,
  [SYS_getpid] = sys_getpid,
  [SYS_sbrk] = sys_sbrk,
  [SYS_sleep] = sys_sleep,
  [SYS_uptime] = sys_uptime,
  [SYS_open] = sys_open,
  [SYS_write] = sys_write,
  [SYS_mknod] = sys_mknod,
  [SYS_unlink] = sys_unlink,
  [SYS_link] = sys_link,
  [SYS_mkdir] = sys_mkdir,
  [SYS_close] = sys_close,
  [SYS_ps] = sys_ps,
  [SYS_tty_attach] = sys_tty_attach,
  [SYS_tty_set_foreground] = sys_tty_set_foreground,
  [SYS_sched_yield] = sys_sched_yield,
  [SYS_vmdump] = sys_vmdump,
  [SYS_mmap] = sys_mmap,
  [SYS_munmap] = sys_munmap,
  [SYS_ioctl] = sys_ioctl,
#ifdef XV6_HAVE_VFS
  [SYS_mount] = sys_mount,
  [SYS_rename] = sys_rename,
#endif
#ifdef XV6_HAVE_NET
  [SYS_udp_bind] = sys_udp_bind,
  [SYS_udp_unbind] = sys_udp_unbind,
  [SYS_udp_send] = sys_udp_send,
  [SYS_udp_recv] = sys_udp_recv,
  [SYS_udp_tryrecv] = sys_udp_tryrecv,
  [SYS_udp_recv_timeout] = sys_udp_recv_timeout,
  [SYS_icmp_send] = sys_icmp_send,
  [SYS_icmp_recv] = sys_icmp_recv,
  [SYS_wifi_connect] = sys_wifi_connect,
  [SYS_net_dhcp] = sys_net_dhcp,
#endif
};
void syscall(void) {
  struct proc *p=myproc(); uint n=p->trapframe->x7;
  p->trapframe->x0=(n<NELEM(calls) && calls[n]) ? calls[n]() : (uint64)-1;
}
