// Allwinner A10 interrupt controller (QEMU "cubieboard"), IRQs 0..95.
// Used by the TINY210_QEMU stand-in build of the S5PV210 port.
#include "kernel/types.h"
#include "kernel/aarch64.h"
#include "kernel/param.h"
#include "kernel/spinlock.h"
#include "kernel/defs.h"
#include "kernel/device.h"
#include "kernel/module.h"
#include "kernel/arch/arm/irq.h"

#define PEND(i)   (0x10 + 4*(i))
#define SELECT(i) (0x30 + 4*(i))
#define ENABLE(i) (0x40 + 4*(i))
#define MASK(i)   (0x50 + 4*(i))
static uintptr base;
#define REG(o) (*(volatile uint32*)(base + (o)))

static void sun4i_unmask(int irq) { REG(ENABLE(irq/32)) |= 1U << (irq%32); REG(MASK(irq/32)) &= ~(1U << (irq%32)); }
static void sun4i_mask(int irq) { REG(MASK(irq/32)) |= 1U << (irq%32); }
static struct irq_chip sun4i_chip = { "sun4i-intc", sun4i_unmask, sun4i_mask };

static void
sun4i_handle_irq(void)
{
  for(int i = 0; i < 3; i++){
    uint32 st = REG(PEND(i)) & REG(ENABLE(i)) & ~REG(MASK(i));
    for(int b = 0; st; b++, st >>= 1)
      if(st & 1)
        generic_handle_irq(32*i + b);
  }
}

static int
sun4i_probe(struct device *dev)
{
  base = (uintptr)dev->resource[0].start;
  for(int i = 0; i < 3; i++){ REG(ENABLE(i)) = 0; REG(MASK(i)) = ~0U; REG(SELECT(i)) = 0; }
  set_handle_irq(sun4i_handle_irq, &sun4i_chip);
  printf("irqchip: sun4i-intc at %x\n", (uint)base);
  return 0;
}

static struct device_driver sun4i_driver = { .name = "sun4i-intc", .bus = &platform_bus, .probe = sun4i_probe };
static void sun4i_init(void) { platform_driver_register(&sun4i_driver); }
arch_initcall("irq-sun4i", sun4i_init);
