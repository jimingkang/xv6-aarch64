// ARM PL192 VIC, four of them on S5PV210 (VIC0..3 at 1 MiB strides),
// flattened to IRQ numbers 0..127 (VICn bit b = 32*n + b).
#include "kernel/types.h"
#include "kernel/aarch64.h"
#include "kernel/param.h"
#include "kernel/spinlock.h"
#include "kernel/defs.h"
#include "kernel/device.h"
#include "kernel/module.h"
#include "kernel/arch/arm/irq.h"

#define VIC_IRQSTATUS    0x000
#define VIC_INTSELECT    0x00c
#define VIC_INTENABLE    0x010
#define VIC_INTENCLEAR   0x014
#define VIC_SOFTINTCLEAR 0x01c
#define VIC_ADDRESS      0xf00
#define NVIC 4
static uintptr vic[NVIC];
#define REG(n, o) (*(volatile uint32*)(vic[n] + (o)))

static void vic_unmask(int irq) { REG(irq/32, VIC_INTENABLE) = 1U << (irq%32); }
static void vic_mask(int irq) { REG(irq/32, VIC_INTENCLEAR) = 1U << (irq%32); }
static struct irq_chip vic_chip = { "vic", vic_unmask, vic_mask };

static void
vic_handle_irq(void)
{
  for(int n = 0; n < NVIC; n++){
    uint32 st = REG(n, VIC_IRQSTATUS);
    for(int b = 0; st; b++, st >>= 1)
      if(st & 1)
        generic_handle_irq(32*n + b);
  }
  // Non-vectored use: VICADDRESS is never read, but writing it keeps the
  // priority logic idle.
  for(int n = 0; n < NVIC; n++)
    REG(n, VIC_ADDRESS) = 0;
}

static int
vic_probe(struct device *dev)
{
  for(int n = 0; n < NVIC; n++){
    vic[n] = (uintptr)dev->resource[0].start + 0x100000U*n;
    REG(n, VIC_INTENCLEAR) = ~0U;   // drop whatever the boot loader enabled
    REG(n, VIC_SOFTINTCLEAR) = ~0U;
    REG(n, VIC_INTSELECT) = 0;      // all IRQ, no FIQ
    REG(n, VIC_ADDRESS) = 0;
  }
  set_handle_irq(vic_handle_irq, &vic_chip);
  printf("irqchip: %d x PL192 VIC at %x\n", NVIC, (uint)dev->resource[0].start);
  return 0;
}

static struct device_driver vic_driver = { .name = "s5pv210-vic", .bus = &platform_bus, .probe = vic_probe };
static void vic_init(void) { platform_driver_register(&vic_driver); }
arch_initcall("irq-vic", vic_init);
