#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "aarch64.h"

// BCM2837 legacy interrupt controller and ARM-local interrupt routing.
// The rest of the kernel keeps the old gic_* interface so trap.c stays small.

#define IRQ_PENDING_1       0x204
#define IRQ_PENDING_2       0x208
#define ENABLE_IRQS_2       0x214
#define DISABLE_IRQS_1      0x21c
#define DISABLE_IRQS_2      0x220
#define DISABLE_BASIC_IRQS  0x224

#define CORE_TIMER_CONTROL(c) (0x40 + 4 * (c))
#define CORE_IRQ_SOURCE(c)    (0x60 + 4 * (c))
#define CORE_VIRTUAL_TIMER    (1 << 3)

static inline uint32
irqread(uint64 off)
{
  return *(volatile uint32 *)(IRQCTRL + off);
}

static inline void
irqwrite(uint64 off, uint32 value)
{
  *(volatile uint32 *)(IRQCTRL + off) = value;
}

static inline uint32
localread(uint64 off)
{
  return *(volatile uint32 *)(LOCAL_BASE + off);
}

static inline void
localwrite(uint64 off, uint32 value)
{
  *(volatile uint32 *)(LOCAL_BASE + off) = value;
}

void
gicv3init(void)
{
  // Start with all legacy GPU interrupts disabled, then enable PL011 UART0.
  irqwrite(DISABLE_IRQS_1, ~0U);
  irqwrite(DISABLE_IRQS_2, ~0U);
  irqwrite(DISABLE_BASIC_IRQS, ~0U);
  irqwrite(ENABLE_IRQS_2, 1U << (UART0_IRQ - 32));
}

void
gicv3inithart(void)
{
  int cpu = cpuid();
  localwrite(CORE_TIMER_CONTROL(cpu), CORE_VIRTUAL_TIMER);
}

uint32
gic_iar(void)
{
  int cpu = cpuid();

  if(localread(CORE_IRQ_SOURCE(cpu)) & CORE_VIRTUAL_TIMER)
    return TIMER0_IRQ;
  if(irqread(IRQ_PENDING_2) & (1U << (UART0_IRQ - 32)))
    return UART0_IRQ;
  if(irqread(IRQ_PENDING_1))
    return 1023;
  return 1023;
}

int
gic_iar_irq(uint32 iar)
{
  return iar;
}

void
gic_eoi(uint32 iar)
{
  (void)iar;
  // BCM2837 interrupt sources are acknowledged at the source device.
}

int
gic_int_enabled(uint32 intid)
{
  if(intid == UART0_IRQ)
    return 1;
  if(intid == TIMER0_IRQ)
    return 1;
  return 0;
}
