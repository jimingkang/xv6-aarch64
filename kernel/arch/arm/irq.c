// ARM32 interrupt dispatch: the root controller driver (irqchip/) reads its
// pending register and calls generic_handle_irq() for each line.
#include "kernel/types.h"
#include "kernel/aarch64.h"
#include "kernel/defs.h"
#include "kernel/arch/arm/irq.h"

static struct {
  irq_handler_t handler;
  void *data;
} irq_desc[NR_IRQS];

static void (*handle_arch_irq)(void);
static struct irq_chip *root_chip;

void
set_handle_irq(void (*handle)(void), struct irq_chip *chip)
{
  if(handle_arch_irq)
    panic("set_handle_irq: second root interrupt controller");
  handle_arch_irq = handle;
  root_chip = chip;
}

int
request_irq(int irq, irq_handler_t handler, void *data)
{
  if(irq < 0 || irq >= NR_IRQS || root_chip == 0 || irq_desc[irq].handler)
    return -1;
  irq_desc[irq].handler = handler;
  irq_desc[irq].data = data;
  root_chip->unmask(irq);
  return 0;
}

void
generic_handle_irq(int irq)
{
  if(irq >= 0 && irq < NR_IRQS && irq_desc[irq].handler){
    irq_desc[irq].handler(irq, irq_desc[irq].data);
    return;
  }
  printf("irq %d: no handler, masked\n", irq);
  if(root_chip && irq >= 0 && irq < NR_IRQS)
    root_chip->mask(irq);
}

void
arch_handle_irq(void)
{
  if(handle_arch_irq)
    handle_arch_irq();
}
