// Tick accounting and clocksource registry for the Cortex-A8 ports.
#include "kernel/types.h"
#include "kernel/aarch64.h"
#include "kernel/param.h"
#include "kernel/spinlock.h"
#include "kernel/defs.h"
#include "kernel/arch/arm/time.h"

struct spinlock tickslock;
uint ticks;

static uint64 (*cs_read)(void);
static uint cs_hz = 24000000;

void
arm32_time_init(void)
{
  initlock(&tickslock, "ticks");
}

void
register_clocksource(char *name, uint64 (*read)(void), uint hz)
{
  cs_read = read;
  cs_hz = hz;
  printf("clocksource: %s at %d Hz\n", name, hz);
}

uint arm32_counter_hz(void) { return cs_hz; }
uint64 arm32_counter(void) { return cs_read ? cs_read() : 0; }

// HZ_TICK interrupts per second; every 10th advances xv6's ticks (10 Hz,
// as the original sleep() granularity expects). The console UART has no
// interrupt of its own here, so input is polled on every tick.
void
arm32_timer_tick(void)
{
  static uint sub;
  if(++sub == 10){
    sub = 0;
    acquire(&tickslock);
    ticks++;
    wakeup(&ticks);
    release(&tickslock);
  }
  uartintr();
  sched_tick();
}
