#include "types.h"
#include "aarch64.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"

uint64
sys_exit(void)
{
  int n;
  if(argint(0, &n) < 0)
    return -1;
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  if(argaddr(0, &p) < 0)
    return -1;
  return wait(p);
}

uint64
sys_sbrk(void)
{
  int addr;
  int n;

  if(argint(0, &n) < 0)
    return -1;
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;

  if(argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(myproc()->killed){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  if(argint(0, &pid) < 0)
    return -1;
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

uint64
sys_getrandom(void)
{
  uint64 destination;
  int length;
  uchar bytes[64];
  int done = 0;

  if(argaddr(0, &destination) < 0 || argint(1, &length) < 0 ||
     length < 0 || length > 4096)
    return -1;
  while(done < length){
    int chunk = length - done;
    if(chunk > sizeof(bytes))
      chunk = sizeof(bytes);
    if(rngbytes(bytes, chunk) != chunk ||
       copyout(myproc()->pagetable, destination + done,
               (char *)bytes, chunk) < 0){
      memset(bytes, 0, sizeof(bytes));
      return -1;
    }
    done += chunk;
  }
  memset(bytes, 0, sizeof(bytes));
  return done;
}

uint64
sys_ps(void)
{
  uint64 dst;
  int size, n;
  char *buf;

  if(argaddr(0, &dst) < 0 || argint(1, &size) < 0 ||
     size <= 0 || size > PGSIZE || (buf = kalloc()) == 0)
    return -1;
  n = proclist(buf, size);
  if(copyout(myproc()->pagetable, dst, buf, n) < 0)
    n = -1;
  kfree(buf);
  return n;
}

uint64
sys_tty_attach(void)
{
  int ttyno;
  if(argint(0, &ttyno) < 0)
    return -1;
  return ttyattach(ttyno);
}

uint64
sys_vmdump(void)
{
  int pid;
  if(argint(0, &pid) < 0 || pid < 0)
    return -1;
  return procvmdump(pid);
}

uint64
sys_sync_create(void)
{
  int type, initial;
  if(argint(0, &type) < 0 || argint(1, &initial) < 0)
    return -1;
  return ksync_create(type, initial);
}

uint64
sys_sync_wait(void)
{
  int handle;
  if(argint(0, &handle) < 0)
    return -1;
  return ksync_wait(handle);
}

uint64
sys_sync_signal(void)
{
  int handle, count;
  if(argint(0, &handle) < 0 || argint(1, &count) < 0)
    return -1;
  return ksync_signal(handle, count);
}

uint64
sys_sync_reset(void)
{
  int handle;
  if(argint(0, &handle) < 0)
    return -1;
  return ksync_reset(handle);
}

uint64
sys_sync_atomic(void)
{
  int handle, op, value, compare;
  if(argint(0, &handle) < 0 || argint(1, &op) < 0 ||
     argint(2, &value) < 0 || argint(3, &compare) < 0)
    return -1;
  return ksync_atomic(handle, op, value, compare);
}

uint64
sys_sync_destroy(void)
{
  int handle;
  if(argint(0, &handle) < 0)
    return -1;
  return ksync_destroy(handle);
}
