// S5PV210 PWM timers: timer 4 is the 100 Hz system tick, timer 3 the
// free-running clocksource behind r_cntvct_el0() (udelay and friends).
// Prescaler 1 (timers 2-4) = 0. Timer 4 counts at PCLK_PSYS (66.7 MHz);
// timer 3 at PCLK_PSYS/16 (4.17 MHz) through its full 32-bit range, so it
// wraps every ~17 minutes and keeps counting with interrupts disabled
// (drivers busy-wait inside spinlocks).
#include "kernel/types.h"
#include "kernel/aarch64.h"
#include "kernel/param.h"
#include "kernel/spinlock.h"
#include "kernel/defs.h"
#include "kernel/device.h"
#include "kernel/module.h"
#include "kernel/clk.h"
#include "kernel/arch/arm/irq.h"
#include "kernel/arch/arm/time.h"

#define TCFG0 0x00
#define TCFG1 0x04
#define TCON 0x08
#define TCNTB3 0x30
#define TCNTO3 0x38
#define TCNTB4 0x3c
#define TCNTO4 0x40
#define TINT_CSTAT 0x44
static uintptr base;
#define REG(o) (*(volatile uint32*)(base + (o)))
static uint reload;

// 64-bit extension of the down-counting 32-bit timer 3.
static uint64
pwm_read(void)
{
  static uint32 last;
  static uint64 high;
  int on = intr_get();
  intr_off();
  uint32 now = 0xffffffffU - REG(TCNTO3);
  if(now < last)
    high += 1ULL << 32;
  last = now;
  uint64 v = high | now;
  if(on) intr_on();
  return v;
}

static void
pwm_irq(int irq, void *data)
{
  uint cstat = REG(TINT_CSTAT);
  if(!(cstat & (1U<<9)))
    return;
  REG(TINT_CSTAT) = (cstat & 0x1f) | (1U<<9);   // W1C, keep the enables
  (void)pwm_read();   // at least one read per wrap of timer 3
  arm32_timer_tick();
}

static int
pwm_probe(struct device *dev)
{
  base = (uintptr)dev->resource[0].start;
  int irq = (int)dev->resource[1].start;
  uint hz = clk_get_rate("pclk_psys");
  if(hz < 10000000 || hz > 200000000)
    hz = 66700000;
  reload = hz / HZ_TICK;
  REG(TCON) &= ~(7U<<20);                         // stop timer 4
  REG(TCON) &= ~(0xfU<<16);                       // stop timer 3
  REG(TCFG0) &= ~(0xffU<<8);
  REG(TCFG1) &= ~(0xfU<<16);
  REG(TCFG1) = (REG(TCFG1) & ~(0xfU<<12)) | (4U<<12);   // timer 3: 1/16
  REG(TCNTB3) = 0xffffffffU;
  REG(TCON) = (REG(TCON) & ~(0xfU<<16)) | (1U<<17);     // timer 3 manual update
  REG(TCON) = (REG(TCON) & ~(0xfU<<16)) | (1U<<19) | (1U<<16); // auto-reload, start
  REG(TCNTB4) = reload - 1;
  REG(TINT_CSTAT) = (REG(TINT_CSTAT) & 0x1f) | (1U<<9);   // clear pending
  REG(TINT_CSTAT) = (REG(TINT_CSTAT) & 0x1f) | (1U<<4);   // enable timer 4 IRQ
  REG(TCON) = (REG(TCON) & ~(7U<<20)) | (1U<<21);          // manual update
  REG(TCON) = (REG(TCON) & ~(7U<<20)) | (1U<<22) | (1U<<20); // auto-reload, start
  register_clocksource("pwm-timer3", pwm_read, hz/16);
  return request_irq(irq, pwm_irq, 0);
}

static struct device_driver pwm_driver = { .name = "s5pv210-pwm", .bus = &platform_bus, .probe = pwm_probe };
static void pwm_init(void) { platform_driver_register(&pwm_driver); }
subsys_initcall("samsung-pwm-timer", pwm_init);
