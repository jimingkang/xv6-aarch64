#ifndef XV6_ARM_IRQ_H
#define XV6_ARM_IRQ_H
// ARM32 interrupt framework (a small Linux genirq): one root interrupt
// controller driver installs handle_arch_irq and irq_chip; drivers then
// request_irq() their line.
#define NR_IRQS 128

typedef void (*irq_handler_t)(int irq, void *data);

struct irq_chip {
  char *name;
  void (*unmask)(int irq);
  void (*mask)(int irq);
};

void set_handle_irq(void (*handle)(void), struct irq_chip *chip);
int  request_irq(int irq, irq_handler_t handler, void *data);
void generic_handle_irq(int irq);
void arch_handle_irq(void);   // called by the exception vector
#endif
