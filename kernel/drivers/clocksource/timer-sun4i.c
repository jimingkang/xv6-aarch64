// Allwinner A10 timer 0 (QEMU "cubieboard") from the 24 MHz oscillator,
// as tick and counter for the TINY210_QEMU stand-in build.
#include "kernel/types.h"
#include "kernel/aarch64.h"
#include "kernel/param.h"
#include "kernel/spinlock.h"
#include "kernel/defs.h"
#include "kernel/device.h"
#include "kernel/module.h"
#include "kernel/arch/arm/irq.h"
#include "kernel/arch/arm/time.h"

#define IRQ_EN 0x00
#define IRQ_STA 0x04
#define T0_CTRL 0x10
#define T0_INTV 0x14
#define T0_CUR 0x18
static uintptr base;
#define REG(o) (*(volatile uint32*)(base + (o)))
#define HZ 24000000U
static uint reload = HZ / HZ_TICK;
static volatile uint64 wraps;

static uint64
sun4i_read(void)
{
  uint64 w; uint elapsed;
  do { w = wraps; elapsed = reload - REG(T0_CUR); } while(w != wraps);
  return w*reload + elapsed;
}

static void
sun4i_timer_irq(int irq, void *data)
{
  if(!(REG(IRQ_STA) & 1))
    return;
  REG(IRQ_STA) = 1;
  wraps++;
  arm32_timer_tick();
}

static int
sun4i_timer_probe(struct device *dev)
{
  base = (uintptr)dev->resource[0].start;
  REG(T0_CTRL) = 0; REG(IRQ_EN) = 0; REG(IRQ_STA) = 0x3f;
  REG(T0_INTV) = reload;
  REG(T0_CTRL) = (1<<2) | (1<<1) | 1;   // OSC24M, reload, enable
  REG(IRQ_EN) = 1;
  register_clocksource("sun4i-timer0", sun4i_read, HZ);
  return request_irq((int)dev->resource[1].start, sun4i_timer_irq, 0);
}

static struct device_driver sun4i_timer_driver = { .name = "sun4i-timer", .bus = &platform_bus, .probe = sun4i_timer_probe };
static void sun4i_timer_init(void) { platform_driver_register(&sun4i_timer_driver); }
subsys_initcall("timer-sun4i", sun4i_timer_init);
