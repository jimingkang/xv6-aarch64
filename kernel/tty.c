// Minimal terminal routing for the single Mini UART terminal.

#include "types.h"
#include "param.h"
#include "aarch64.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "fs.h"
#include "file.h"
#include "defs.h"
#include "device.h"

static struct spinlock ttyfg_lock;
static int ttyfg_pgid;

static int
ttycurrentread(int user_dst, uint64 dst, int n)
{
  struct proc *p = myproc();
  if(p == 0 || p->ctty != TTYS0)
    return -1;
  return consoleread(user_dst, dst, n);
}

static int
ttycurrentwrite(int user_src, uint64 src, int n)
{
  struct proc *p = myproc();
  if(p == 0 || p->ctty != TTYS0)
    return -1;
  return consolewrite(user_src, src, n);
}

int
ttyattach(int ttyno)
{
  struct proc *p = myproc();
  if(p == 0 || ttyno != 0)
    return -1;
  // The caller becomes a session and process-group leader. Children inherit
  // these values and ctty in fork(), so /dev/tty remains session-relative.
  p->sid = p->pid;
  p->pgid = p->pid;
  p->ctty = TTYS0;
  acquire(&ttyfg_lock);
  ttyfg_pgid = p->pgid;
  release(&ttyfg_lock);
  return 0;
}

int
ttysetforeground(int pid)
{
  struct proc *caller = myproc();
  struct proc *target = 0;

  if(caller == 0 || caller->ctty != TTYS0)
    return -1;
  if(pid == 0){
    acquire(&ttyfg_lock);
    ttyfg_pgid = 0;
    release(&ttyfg_lock);
    return 0;
  }
  for(struct proc *p = proc_head; p; p = p->next){
    acquire(&p->lock);
    if(p->pid == pid && p->state != UNUSED &&
       p->sid == caller->sid && p->ctty == TTYS0){
      p->pgid = pid;
      target = p;
      release(&p->lock);
      break;
    }
    release(&p->lock);
  }
  if(target == 0)
    return -1;
  acquire(&ttyfg_lock);
  ttyfg_pgid = pid;
  release(&ttyfg_lock);
  return 0;
}

void
ttyintr(void)
{
  int pgid;
  acquire(&ttyfg_lock);
  pgid = ttyfg_pgid;
  release(&ttyfg_lock);
  signal_pgrp(pgid, SIGINT);
}

void
ttyinit(void)
{
  static struct file_operations ttyS0_fops = {
    .read = consoleread,
    .write = consolewrite,
  };
  static struct file_operations tty_fops = {
    .read = ttycurrentread,
    .write = ttycurrentwrite,
  };
  initlock(&ttyfg_lock, "tty foreground");
  // ttyS0 is the concrete terminal. /dev/tty dispatches through p->ctty.
  register_chrdev(TTYS0, "ttyS0", &ttyS0_fops);
  register_chrdev(TTY, "tty", &tty_fops);
}
