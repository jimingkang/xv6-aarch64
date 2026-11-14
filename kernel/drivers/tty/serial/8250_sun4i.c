// 16550-compatible UART with 32-bit register stride (Allwinner A10 UART0,
// QEMU "cubieboard"), console UART of the X210_QEMU stand-in build.
#include "kernel/types.h"
#include "kernel/aarch64.h"
#include "kernel/param.h"
#include "kernel/spinlock.h"
#include "kernel/defs.h"
#include "kernel/device.h"
#include "kernel/module.h"

static uintptr base = EARLY_UART;
#define REG(o) (*(volatile uint32*)(base + (o)))
#define RBR 0x00
#define IER 0x04
#define LCR 0x0c
#define LSR 0x14

void uartinit(void) { REG(LCR) &= ~128U; REG(IER) = 0; }
int uartgetc(void) { return (REG(LSR) & 1) ? (int)(REG(RBR) & 255) : -1; }
void uartputc_sync(int c) { while(!(REG(LSR) & 32)) ; REG(RBR) = (uchar)c; }
void uartputc(int c) { uartputc_sync(c); }
void uartintr(void) { int c; while((c = uartgetc()) >= 0) consoleintr(c); }
void boot_uart_mark(int c) { (void)c; }

static int
uart8250_probe(struct device *dev)
{
  base = (uintptr)dev->resource[0].start;
  uartinit();
  printf("serial: 8250 (sun4i) at %x (console)\n", (uint)base);
  return 0;
}

static struct device_driver uart8250_driver = { .name = "sun4i-uart", .bus = &platform_bus, .probe = uart8250_probe };
static void uart8250_init(void) { platform_driver_register(&uart8250_driver); }
module_init("serial-8250-sun4i", uart8250_init);
