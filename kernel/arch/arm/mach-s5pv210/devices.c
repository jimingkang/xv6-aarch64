/* Board devices of the S5PV210 port (the role of Linux's
   arch/arm/mach-s5pv210/mach-*.c): one platform device per controller,
   with its registers and interrupt as resources. Drivers under
   kernel/drivers/ bind to them by name (platform bus) from do_initcalls().
   s5pv210_map_io() gives every MEM resource a kernel mapping.

   X210_QEMU builds the same port for QEMU's Cortex-A8 "cubieboard",
   whose Allwinner A10 peripherals have their own drivers. */
#include "kernel/types.h"
#include "kernel/param.h"
#include "kernel/memlayout.h"
#include "kernel/aarch64.h"
#include "kernel/spinlock.h"
#include "kernel/defs.h"
#include "kernel/device.h"
#include "kernel/drivers/video/fbdev/s3c-fb.h"
#include "devices.h"

#define MEM(a, sz, n) { (a), (a)+(sz)-1, IORESOURCE_MEM, n }
#define IRQ(i, n)     { (i), (i), IORESOURCE_IRQ, n }

#ifdef X210_QEMU
static struct device board_devices[] = {
  { .name = "sun4i-intc",  .resource = { MEM(0x01C20400, 0x400, "intc") }, .nresource = 1 },
  { .name = "sun4i-timer", .resource = { MEM(0x01C20C00, 0x400, "timer"), IRQ(22, "timer0") }, .nresource = 2 },
  { .name = "sun4i-uart",  .resource = { MEM(0x01C28000, 0x400, "uart0") }, .nresource = 1 },
};
#else
#define REG(a) (*(volatile uint32*)(uintptr)(a))

/* FriendlyARM panels (mini210/tiny210 BSP timings); chosen with
   make S5PV210_LCD=s70|h43|w50|a70. DE-mode panels mostly ignore the
   sync widths; totals and pixel clock matter. */
static const struct s3c_fb_panel panels[] = {
  {"S70", 800, 480, 60,  80, 36, 10, 22, 15,  8, 1, 1, 1, 0},
  {"H43", 480, 272, 60,   5, 40,  2,  8,  8,  2, 1, 1, 1, 0},
  {"W50", 800, 480, 60,  40, 40, 48, 20, 20, 12, 1, 1, 1, 0},
  {"A70", 800, 480, 60,  40, 40, 48, 17, 29, 24, 1, 1, 1, 0},
};
#ifndef S5PV210_LCD_INDEX
#define S5PV210_LCD_INDEX 0
#endif

/* GPF0[7:0], GPF1[7:0], GPF2[7:0], GPF3[3:0] = LCD (function 2) at the
   strongest drive; DISPLAY_CONTROL[1:0] = 2 routes FIMD to the RGB pins. */
static void
fb_setup_gpio(void)
{
  for(int n = 0; n < 3; n++){
    REG(0xE0200120 + 0x20*n) = 0x22222222;
    REG(0xE020012C + 0x20*n) = 0xffffffff;
  }
  REG(0xE0200180) = (REG(0xE0200180) & ~0xffffU) | 0x2222;
  REG(0xE020018C) |= 0xff;
  REG(0xE0100464) |= 1;                                  /* CLK_GATE_IP1: FIMD */
  REG(0xE0107008) = (REG(0xE0107008) & ~3U) | 2;         /* DISPLAY_CONTROL */
}
static struct s3c_fb_platdata fb_pdata = { &panels[S5PV210_LCD_INDEX], fb_setup_gpio };

static struct device board_devices[] = {
  { .name = "s5pv210-clock", .resource = { MEM(0xE0100000, 0x10000, "clock") }, .nresource = 1 },
  { .name = "s3c2410-wdt",   .resource = { MEM(0xE2700000, 0x400, "wdt") }, .nresource = 1 },
  { .name = "s5pv210-vic",   .resource = { MEM(0xF2000000, 0x400000, "vic0-3") }, .nresource = 1 },
  { .name = "s5pv210-pwm",   .resource = { MEM(0xE2500000, 0x400, "pwm"), IRQ(25, "timer4") }, .nresource = 2 },
  { .name = "s5pv210-uart",  .id = BOARD_UART_N,
    .resource = { MEM(BOARD_UART, 0x400, "uart"), IRQ(42 + BOARD_UART_N, "uart") }, .nresource = 2 },
  { .name = "s5pv210-gpio",  .resource = { MEM(0xE0200000, 0x1000, "gpio") }, .nresource = 1 },
  { .name = "s3c-fb", .driver_data = &fb_pdata,
    .resource = { MEM(0xF8000000, 0x10000, "fimd"),
                  MEM(DRAM_BASE + S5PV210_FB_OFFSET, S5PV210_FB_SIZE, "framebuffer") },
    .nresource = 2 },
};
#endif
#define NDEVICES (sizeof(board_devices)/sizeof(board_devices[0]))

#define SECTION_DEV(pa) ((pa) | (1<<10) | (1<<4) | 2)            /* priv RW, XN, strongly ordered */
#define SECTION_NC(pa)  ((pa) | (1<<12) | (1<<10) | (1<<4) | 2)  /* priv RW, XN, Normal non-cacheable */

/* Called by kvminit() (kernel/arch/arm/vm.c): 1 MiB sections for every MEM
   resource; the frame buffer is Normal memory, the rest device memory. */
void
x210_map_devices(pagetable_t p)
{
  for(uint i = 0; i < NDEVICES; i++)
    for(int r = 0; r < board_devices[i].nresource; r++){
      struct resource *res = &board_devices[i].resource[r];
      if(res->type != IORESOURCE_MEM)
        continue;
      int nc = res->name && strncmp(res->name, "framebuffer", 12) == 0;
      for(uint a = (uint)res->start & ~0xFFFFFU; a <= (uint)res->end; a += 0x100000)
        p[a>>20] = nc ? SECTION_NC(a) : SECTION_DEV(a);
    }
}

void
s5pv210_register_devices(void)
{
  for(uint i = 0; i < NDEVICES; i++)
    if(platform_device_register(&board_devices[i]) < 0)
      printf("board: cannot register %s\n", board_devices[i].name);
}
