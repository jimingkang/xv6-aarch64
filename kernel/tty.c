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
  return 0;
}

void
ttyinit(void)
{
  // ttyS0 is the concrete terminal. /dev/tty dispatches through p->ctty.
  devsw[TTYS0].read = consoleread;
  devsw[TTYS0].write = consolewrite;
  devsw[TTY].read = ttycurrentread;
  devsw[TTY].write = ttycurrentwrite;
}
