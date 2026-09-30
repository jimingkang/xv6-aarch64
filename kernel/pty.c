#include "types.h"
#include "aarch64.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "epoll.h"

// A PTY contains two independent byte streams:
// master -> input -> slave, and slave -> output -> master.
#define PTYSIZE 512
#define PTY_LINE 128
#define PTY_HISTORY 8

struct pty_queue {
  char data[PTYSIZE];
  uint nread;
  uint nwrite;
};

struct pty {
  struct spinlock lock;
  struct pty_queue input;
  struct pty_queue output;
  int masteropen;
  int slaveopen;
  char edit[PTY_LINE];
  uint editlen;
  char history[PTY_HISTORY][PTY_LINE];
  uint history_len[PTY_HISTORY];
  int history_count;
  int history_pos;
  int escape_state;
  int saw_input_cr;
  int saw_output_cr;
};

// Caller holds pt->lock. Queue one byte, sleeping only while the peer exists.
static int
ptyqput(struct pty *pt, struct pty_queue *q, int *readeropen, char ch)
{
  struct proc *p = myproc();
  while(q->nwrite == q->nread + PTYSIZE){
    if(!*readeropen || p->killed)
      return -1;
    wakeup(&q->nread);
    sleep(&q->nwrite, &pt->lock);
  }
  if(!*readeropen)
    return -1;
  q->data[q->nwrite++ % PTYSIZE] = ch;
  return 0;
}

static int
ptyecho(struct pty *pt, char ch)
{
  return ptyqput(pt, &pt->output, &pt->masteropen, ch);
}

static void
ptyerase(struct pty *pt)
{
  ptyecho(pt, '\b');
  ptyecho(pt, ' ');
  ptyecho(pt, '\b');
}

static void
ptyreplace(struct pty *pt, int history_pos)
{
  while(pt->editlen){
    pt->editlen--;
    ptyerase(pt);
  }
  if(history_pos >= 0 && history_pos < pt->history_count){
    pt->editlen = pt->history_len[history_pos];
    memmove(pt->edit, pt->history[history_pos], pt->editlen);
    for(uint i = 0; i < pt->editlen; i++)
      ptyecho(pt, pt->edit[i]);
  }
}

static void
ptyhistorysave(struct pty *pt)
{
  if(pt->editlen == 0)
    return;
  if(pt->history_count > 0 &&
     pt->history_len[pt->history_count - 1] == pt->editlen &&
     memcmp(pt->history[pt->history_count - 1], pt->edit,
            pt->editlen) == 0)
    return;
  if(pt->history_count == PTY_HISTORY){
    for(int i = 1; i < PTY_HISTORY; i++){
      pt->history_len[i - 1] = pt->history_len[i];
      memmove(pt->history[i - 1], pt->history[i], PTY_LINE);
    }
    pt->history_count--;
  }
  memmove(pt->history[pt->history_count], pt->edit, pt->editlen);
  pt->history_len[pt->history_count] = pt->editlen;
  pt->history_count++;
}

// Minimal canonical line discipline for bytes arriving from the SSH side.
// ESC [ A/B recalls one of eight commands and redraws the current line.
static int
ptyinput(struct pty *pt, char ch)
{
  if(pt->escape_state == 1){
    pt->escape_state = ch == '[' ? 2 : 0;
    return 0;
  }
  if(pt->escape_state == 2){
    pt->escape_state = 0;
    if(ch == 'A' && pt->history_count > 0){
      if(pt->history_pos > 0)
        pt->history_pos--;
      ptyreplace(pt, pt->history_pos);
    } else if(ch == 'B' && pt->history_count > 0){
      if(pt->history_pos < pt->history_count - 1){
        pt->history_pos++;
        ptyreplace(pt, pt->history_pos);
      } else {
        pt->history_pos = pt->history_count;
        ptyreplace(pt, -1);
      }
    }
    return 0;
  }
  if((uchar)ch == 0x1b){
    pt->escape_state = 1;
    return 0;
  }
  if(ch == '\n' && pt->saw_input_cr){
    pt->saw_input_cr = 0;
    return 0;
  }
  pt->saw_input_cr = ch == '\r';
  if(ch == '\r' || ch == '\n'){
    ptyhistorysave(pt);
    for(uint i = 0; i < pt->editlen; i++)
      if(ptyqput(pt, &pt->input, &pt->slaveopen, pt->edit[i]) < 0)
        return -1;
    if(ptyqput(pt, &pt->input, &pt->slaveopen, '\n') < 0)
      return -1;
    pt->editlen = 0;
    pt->history_pos = pt->history_count;
    ptyecho(pt, '\r');
    ptyecho(pt, '\n');
    wakeup(&pt->input.nread);
    return 0;
  }
  if(ch == '\b' || (uchar)ch == 0x7f){
    if(pt->editlen){
      pt->editlen--;
      ptyerase(pt);
    }
    return 0;
  }
  if((uchar)ch >= ' ' && pt->editlen + 1 < PTY_LINE){
    pt->edit[pt->editlen++] = ch;
    ptyecho(pt, ch);
  }
  return 0;
}

int
ptyalloc(struct file **master, struct file **slave)
{
  struct pty *pt = 0;

  *master = *slave = 0;
  if((*master = filealloc()) == 0 || (*slave = filealloc()) == 0)
    goto bad;
  if((pt = (struct pty *)kalloc()) == 0)
    goto bad;
  memset(pt, 0, sizeof(*pt));
  initlock(&pt->lock, "pty");
  pt->masteropen = pt->slaveopen = 1;

  (*master)->type = FD_PTY;
  (*master)->readable = (*master)->writable = 1;
  (*master)->pty = pt;
  (*master)->pty_master = 1;
  (*slave)->type = FD_PTY;
  (*slave)->readable = (*slave)->writable = 1;
  (*slave)->pty = pt;
  (*slave)->pty_master = 0;
  return 0;

bad:
  if(pt)
    kfree(pt);
  if(*master)
    fileclose(*master);
  if(*slave)
    fileclose(*slave);
  return -1;
}

void
ptyclose(struct pty *pt, int master)
{
  acquire(&pt->lock);
  if(master)
    pt->masteropen = 0;
  else
    pt->slaveopen = 0;
  wakeup(&pt->input.nread);
  wakeup(&pt->input.nwrite);
  wakeup(&pt->output.nread);
  wakeup(&pt->output.nwrite);
  epollnotify();
  if(!pt->masteropen && !pt->slaveopen){
    release(&pt->lock);
    kfree(pt);
  } else {
    release(&pt->lock);
  }
}

int
ptyread(struct pty *pt, int master, uint64 addr, int n)
{
  struct pty_queue *q = master ? &pt->output : &pt->input;
  struct proc *p = myproc();
  int *writeropen = master ? &pt->slaveopen : &pt->masteropen;
  int i;

  acquire(&pt->lock);
  while(q->nread == q->nwrite && *writeropen){
    if(p->killed){
      release(&pt->lock);
      return -1;
    }
    sleep(&q->nread, &pt->lock);
  }
  for(i = 0; i < n && q->nread != q->nwrite; i++){
    char ch = q->data[q->nread++ % PTYSIZE];
    if(copyout(p->pagetable, addr + i, &ch, 1) < 0)
      break;
  }
  wakeup(&q->nwrite);
  release(&pt->lock);
  epollnotify();
  return i;
}

int
ptywrite(struct pty *pt, int master, uint64 addr, int n)
{
  struct proc *p = myproc();
  int *readeropen = master ? &pt->slaveopen : &pt->masteropen;
  int i = 0;

  acquire(&pt->lock);
  while(i < n){
    char ch;
    if(!*readeropen || p->killed){
      release(&pt->lock);
      return i ? i : -1;
    }
    if(copyin(p->pagetable, &ch, addr + i, 1) < 0)
      break;
    if(master){
      if(ptyinput(pt, ch) < 0)
        break;
    } else {
      // Minimal OPOST/ONLCR: remote terminals need CRLF, while avoiding an
      // extra CR when an application already emitted a CRLF pair.
      if(ch == '\n' && !pt->saw_output_cr &&
         ptyqput(pt, &pt->output, &pt->masteropen, '\r') < 0)
        break;
      if(ptyqput(pt, &pt->output, &pt->masteropen, ch) < 0)
        break;
      pt->saw_output_cr = ch == '\r';
    }
    i++;
  }
  wakeup(&pt->input.nread);
  wakeup(&pt->output.nread);
  release(&pt->lock);
  epollnotify();
  return i;
}

int
ptypoll(struct pty *pt, int master, int events)
{
  struct pty_queue *readq = master ? &pt->output : &pt->input;
  struct pty_queue *writeq = master ? &pt->input : &pt->output;
  int peeropen, ready = 0;

  acquire(&pt->lock);
  peeropen = master ? pt->slaveopen : pt->masteropen;
  if((events & EPOLLIN) && (readq->nread != readq->nwrite || !peeropen))
    ready |= EPOLLIN;
  if((events & EPOLLOUT) && peeropen &&
     writeq->nwrite != writeq->nread + PTYSIZE)
    ready |= EPOLLOUT;
  if(!peeropen)
    ready |= EPOLLERR;
  release(&pt->lock);
  return ready;
}
