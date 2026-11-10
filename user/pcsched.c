// pcsched: producer/consumer over a pipe, reporting where each step ran.
//
//   pcsched [ITEMS] [CONSUMER_MS] [PRODUCER_MS]     defaults 8 30 0
//
// The pipe is the bounded buffer: each item is ITEM_SIZE bytes and the
// kernel pipe holds 512, so SLOTS items fit.  A full pipe makes the producer
// sleep in pipewrite() (chan &pi->nwrite); an empty one makes the consumer
// sleep in piperead() (chan &pi->nread).  The other side's wakeup() makes it
// RUNNABLE again, and whichever CPU is idle first runs it.
//
// While running, neither process prints: console output would add its own
// sleeps (uart) and, from two CPUs at once, interleave character by
// character.  Each process records events (time, CPU, what) in memory.  At
// the end the consumer sends its events to the parent over a second pipe,
// and the parent prints both lists merged by time.  "moved cpuA->cpuB"
// marks a process continuing on a different CPU.  A write()/read() that took
// 2 ms or more is annotated: if the pipe was full (write) or empty (read) when
// it started, it slept in pipewrite()/piperead(); otherwise the time went to
// something else (timer preemption, a page fault, or -- with the kernel
// sched trace on -- the trace records being printed on the slow uart).
//
// With PROC_SCHED_TRACE=1 PROC_SCHED_TRACE_ALL=1 the kernel's sched-trace /
// sched-wake records for pids P and C appear above the report; match them by
// pid and by order (pipewrite / piperead in the switch-out backtraces).
#include <uapi/types.h>
#include "user/user.h"
#include "user/schedutil.h"

#define PIPE_BYTES 512
#define ITEM_SIZE  128
#define SLOTS      (PIPE_BYTES / ITEM_SIZE)
#define MAXEV      64

enum { E_START = 1, E_PUT_BEGIN, E_PUT_END, E_GET_BEGIN, E_GET_END, E_EOF,
       E_WORK_END, E_DONE };

struct ev {
  uint32 t_us;        // since t0
  uint8 cpu;
  uint8 role;         // 'P' or 'C'
  uint8 kind;
  uint8 item;
};

static uint64 t0;
static struct ev evs[MAXEV];
static int nev;

static void
rec(char role, int kind, int item)
{
  if(nev < MAXEV){
    evs[nev].t_us = (uint32)(clock_us() - t0);
    evs[nev].cpu = (uint8)getcpu();
    evs[nev].role = (uint8)role;
    evs[nev].kind = (uint8)kind;
    evs[nev].item = (uint8)item;
    nev++;
  }
}

static void
busy_ms(int ms)                       // CPU-bound work: never sleeps
{
  uint64 until = clock_us() + (uint64)ms * 1000;
  while(clock_us() < until)
    ;
}

static void
producer(int wfd, int items, int work_ms)
{
  char item[ITEM_SIZE];

  rec('P', E_START, 0);
  for(int i = 1; i <= items; i++){
    if(work_ms) busy_ms(work_ms);
    for(int k = 0; k < ITEM_SIZE; k++) item[k] = (char)i;
    rec('P', E_PUT_BEGIN, i);
    if(write(wfd, item, ITEM_SIZE) != ITEM_SIZE)
      exit(1);
    rec('P', E_PUT_END, i);
  }
  close(wfd);
  rec('P', E_DONE, 0);
}

static void
consumer(int rfd, int work_ms)
{
  char item[ITEM_SIZE];

  rec('C', E_START, 0);
  for(;;){
    int got = 0, n;
    rec('C', E_GET_BEGIN, 0);
    while(got < ITEM_SIZE && (n = read(rfd, item + got, ITEM_SIZE - got)) > 0)
      got += n;
    if(got < ITEM_SIZE){
      rec('C', E_EOF, 0);
      return;
    }
    rec('C', E_GET_END, (uchar)item[0]);
    if(work_ms){
      busy_ms(work_ms);
      rec('C', E_WORK_END, (uchar)item[0]);
    }
  }
}

// ---- report ----
struct line { char b[160]; int n; };
static void put_s(struct line *l, const char *s)
{ while(*s && l->n < (int)sizeof(l->b) - 1) l->b[l->n++] = *s++; }
static void put_u(struct line *l, uint64 v, int w)
{
  char t[24]; int i = 0;
  do { t[i++] = '0' + v % 10; v /= 10; } while(v);
  while(i < w) t[i++] = '0';
  while(i > 0 && l->n < (int)sizeof(l->b) - 1) l->b[l->n++] = t[--i];
}
static void put_pad(struct line *l, uint64 v, int w)  // right-aligned
{
  int d = 1; for(uint64 x = v; x >= 10; x /= 10) d++;
  while(d++ < w) put_s(l, " ");
  put_u(l, v, 1);
}

int
main(int argc, char *argv[])
{
  int items = 8, cons_ms = 30, prod_ms = 0, data[2], back[2], pid;
  static struct ev cev[MAXEV];
  int ncev = 0;

  if(argc > 4 || (argc > 1 && !parse_num(argv[1], &items)) ||
     (argc > 2 && !parse_num(argv[2], &cons_ms)) ||
     (argc > 3 && !parse_num(argv[3], &prod_ms)) || items < 1 || items > 20){
    fprintf(2, "usage: pcsched [ITEMS 1..20] [CONSUMER_MS] [PRODUCER_MS]\n");
    exit(1);
  }
  if(pipe(data) < 0 || pipe(back) < 0){
    fprintf(2, "pcsched: pipe failed\n");
    exit(1);
  }
  printf("pcsched: %d items x %d bytes, pipe holds %d; consumer works %d ms,"
         " producer %d ms per item\n", items, ITEM_SIZE, SLOTS, cons_ms, prod_ms);
  t0 = clock_us();

  pid = fork();
  if(pid < 0){
    fprintf(2, "pcsched: fork failed\n");
    exit(1);
  }
  if(pid == 0){                        // child = consumer
    close(data[1]); close(back[0]);
    consumer(data[0], cons_ms);
    rec('C', E_DONE, 0);
    write(back[1], evs, nev * sizeof(struct ev));
    exit(0);
  }
  close(data[0]); close(back[1]);      // parent = producer
  int ppid = getpid();
  producer(data[1], items, prod_ms);
  int n;
  while((n = read(back[0], (char *)cev + ncev * sizeof(struct ev),
                  (MAXEV - ncev) * sizeof(struct ev))) > 0)
    ncev += n / sizeof(struct ev);
  wait(0);

  printf("producer = pid %d, consumer = pid %d; time ms since fork\n", ppid, pid);
  printf("    time   cpu  who  event\n");
  int i = 0, j = 0, lastcpu[2] = { -1, -1 };
  uint32 begin_t[2] = { 0, 0 };
  int wdone = 0, rdone = 0, pipe_wait[2] = { 0, 0 };   // items written / read so far
  while(i < nev || j < ncev){
    struct ev *e = (j >= ncev || (i < nev && evs[i].t_us <= cev[j].t_us)) ? &evs[i++] : &cev[j++];
    int w = e->role == 'C';
    struct line l; l.n = 0;
    put_pad(&l, e->t_us / 1000, 5); put_s(&l, "."); put_u(&l, e->t_us % 1000, 3);
    put_s(&l, "  cpu"); put_u(&l, e->cpu, 1);
    put_s(&l, w ? "  C    " : "  P  ");
    switch(e->kind){
    case E_START:     put_s(&l, w ? "start (forked child)" : "start"); break;
    case E_PUT_BEGIN: put_s(&l, "write #"); put_u(&l, e->item, 1); put_s(&l, " ..."); begin_t[0] = e->t_us; pipe_wait[0] = wdone - rdone >= SLOTS; break;
    case E_PUT_END:   put_s(&l, "write #"); put_u(&l, e->item, 1); put_s(&l, " done"); wdone++; break;
    case E_GET_BEGIN: put_s(&l, "read ..."); begin_t[1] = e->t_us; pipe_wait[1] = wdone == rdone; break;
    case E_GET_END:   put_s(&l, "read  #"); put_u(&l, e->item, 1); put_s(&l, " done"); rdone++; break;
    case E_WORK_END:  put_s(&l, "worked on #"); put_u(&l, e->item, 1); put_s(&l, " ("); put_u(&l, cons_ms, 1); put_s(&l, " ms busy)"); break;
    case E_EOF:       put_s(&l, "read EOF (producer closed)"); break;
    case E_DONE:      put_s(&l, "done"); break;
    }
    if((e->kind == E_PUT_END || e->kind == E_GET_END || e->kind == E_EOF) &&
       e->t_us - begin_t[w] >= 2000){
      put_s(&l, "   <- ~"); put_u(&l, (e->t_us - begin_t[w]) / 1000, 1);
      if(pipe_wait[w])
        put_s(&l, w ? " ms asleep in piperead (pipe was empty)"
                    : " ms asleep in pipewrite (pipe was full)");
      else
        put_s(&l, " ms, pipe was not full/empty: preempt/fault/trace print");
    }
    if(lastcpu[w] >= 0 && lastcpu[w] != e->cpu){
      put_s(&l, "   [moved cpu"); put_u(&l, lastcpu[w], 1); put_s(&l, "->cpu");
      put_u(&l, e->cpu, 1); put_s(&l, "]");
    }
    lastcpu[w] = e->cpu;
    l.b[l.n++] = '\n';
    write(1, l.b, l.n);
  }
  printf("pcsched: done\n");
  exit(0);
}
