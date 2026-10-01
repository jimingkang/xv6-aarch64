#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "aarch64.h"
#include "defs.h"
#include "net.h"

// armv8 generic timer driver

#define CNTV_CTL_ENABLE   (1<<0)
#define CNTV_CTL_IMASK    (1<<1)
#define CNTV_CTL_ISTATUS  (1<<2)

static void enable_timer(void);
static void disable_timer(void);
static void reload_timer(void);
static uchar timer_divider[NCPU];

void
timerinit()
{
  disable_timer();
  reload_timer();
  enable_timer();
}

static void
enable_timer()
{
  uint64 c = r_cntv_ctl_el0();
  c |= CNTV_CTL_ENABLE;
  c &= ~CNTV_CTL_IMASK;
  w_cntv_ctl_el0(c);
}

static void
disable_timer()
{
  uint64 c = r_cntv_ctl_el0();
  c &= ~CNTV_CTL_ENABLE;
  c |= CNTV_CTL_IMASK;
  w_cntv_ctl_el0(c);
}

static void
reload_timer()
{
  // Poll network devices every 10 ms.  Ten hardware interrupts still form
  // one xv6 tick, preserving sleep(n), uptime and 100 ms preemption.
  uint64 interval = 10000;
  uint64 interval_clk = interval * (r_cntfrq_el0() / 1000000);

  w_cntv_tval_el0(interval_clk);
}

void
delay(uint32 cycles)
{
  uint64 start = r_cntvct_el0();

  while ((r_cntvct_el0() - start) < cycles)
    asm volatile("yield" ::: "memory");
}

int
timerintr()
{
  int cpu = cpuid();
  int logical_tick;

  disable_timer();
  reload_timer();
  timer_divider[cpu]++;
  logical_tick = timer_divider[cpu] == 10;
  if(logical_tick)
    timer_divider[cpu] = 0;
  // Every CPU owns a Generic Timer, but shared network state is maintained by
  // one schedulable kworker.  The hard IRQ only queues work; it never performs
  // SDIO/USB transfers or enters the network stack.
  if(cpu == 0){
    net_deferred_schedule(logical_tick);
  }
  enable_timer();
  return logical_tick;
}
