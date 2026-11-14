// Samsung S3C/S5P UART (S5PV210 UART0..3) as the console UART.
// The kernel console (kernel/console.c) calls uartputc_sync()/uartgetc()
// directly, also before any driver has probed, so the base starts at the
// board's EARLY_UART (BL1/U-Boot already set the pins and 115200 8N1) and
// the platform driver only takes the address from its resource and masks
// the UART's own interrupt sources; input is polled on each timer tick.
#include "kernel/types.h"
#include "kernel/aarch64.h"
#include "kernel/param.h"
#include "kernel/spinlock.h"
#include "kernel/defs.h"
#include "kernel/device.h"
#include "kernel/module.h"

#define UFCON 0x08
#define UTRSTAT 0x10
#define UFSTAT 0x18
#define UTXH 0x20
#define URXH 0x24
#define UINTP 0x30
#define UINTSP 0x34
#define UINTM 0x38
static uintptr base = EARLY_UART;
#define REG(o) (*(volatile uint32*)(base + (o)))

void fbcon_putc(int c) __attribute__((weak));

void uartinit(void) { REG(UINTM) = 0xf; REG(UINTSP) = 0xf; REG(UINTP) = 0xf; }
static int fifo(void) { return REG(UFCON) & 1; }
int
uartgetc(void)
{
  int ready = fifo() ? (REG(UFSTAT) & 0x1ff) != 0 : (REG(UTRSTAT) & 1);
  return ready ? (int)(REG(URXH) & 255) : -1;
}
void
uartputc_sync(int c)
{
  if(fifo()) { while(REG(UFSTAT) & (1U<<24)) ; }   // Tx FIFO full
  else { while(!(REG(UTRSTAT) & 2)) ; }            // Tx buffer empty
  REG(UTXH) = (uchar)c;
  if(fbcon_putc)
    fbcon_putc(c);                                 // mirror on the LCD
}
void uartputc(int c) { uartputc_sync(c); }
void uartintr(void) { int c; while((c = uartgetc()) >= 0) consoleintr(c); }
void boot_uart_mark(int c) { (void)c; }

static int
samsung_uart_probe(struct device *dev)
{
  base = (uintptr)dev->resource[0].start;
  uartinit();
  printf("serial: s5pv210-uart at %x (console)\n", (uint)base);
  return 0;
}

static struct device_driver samsung_uart_driver = { .name = "s5pv210-uart", .bus = &platform_bus, .probe = samsung_uart_probe };
static void samsung_uart_init(void) { platform_driver_register(&samsung_uart_driver); }
module_init("serial-samsung", samsung_uart_init);
