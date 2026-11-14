// S5PV210 PWM timer 4 as the system tick and free-running counter.
// Timer 4 has no output pin; prescaler 1 = 0 and divider 1/1 make it count
// at PCLK_PSYS (66.7 MHz). Counters are 32-bit, so a 10 ms reload fits.
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
#define TCNTB4 0x3c
#define TCNTO4 0x40
#define TINT_CSTAT 0x44
static uintptr base;
#define REG(o) (*(volatile uint32*)(base + (o)))
static uint reload;
static volatile uint64 wraps;

static uint64
pwm_read(void)
{
  uint64 w; uint elapsed;
  do { w = wraps; elapsed = (reload-1) - REG(TCNTO4); } while(w != wraps);
  return w*reload + elapsed;
}

static void
pwm_irq(int irq, void *data)
{
  uint cstat = REG(TINT_CSTAT);
  if(!(cstat & (1U<<9)))
    return;
  REG(TINT_CSTAT) = (cstat & 0x1f) | (1U<<9);   // W1C, keep the enables
  wraps++;
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
  REG(TCFG0) &= ~(0xffU<<8);
  REG(TCFG1) &= ~(0xfU<<16);
  REG(TCNTB4) = reload - 1;
  REG(TINT_CSTAT) = (REG(TINT_CSTAT) & 0x1f) | (1U<<9);   // clear pending
  REG(TINT_CSTAT) = (REG(TINT_CSTAT) & 0x1f) | (1U<<4);   // enable timer 4 IRQ
  REG(TCON) = (REG(TCON) & ~(7U<<20)) | (1U<<21);          // manual update
  REG(TCON) = (REG(TCON) & ~(7U<<20)) | (1U<<22) | (1U<<20); // auto-reload, start
  register_clocksource("pwm-timer4", pwm_read, hz);
  return request_irq(irq, pwm_irq, 0);
}

static struct device_driver pwm_driver = { .name = "s5pv210-pwm", .bus = &platform_bus, .probe = pwm_probe };
static void pwm_init(void) { platform_driver_register(&pwm_driver); }
subsys_initcall("samsung-pwm-timer", pwm_init);
