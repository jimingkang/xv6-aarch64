/* Board devices of the S5PV210 port (the role of Linux's
   arch/arm/mach-s5pv210/mach-*.c): one platform device per controller,
   with its registers and interrupt as resources. Drivers under
   kernel/drivers/ bind to them by name (platform bus) from do_initcalls().
   s5pv210_map_io() gives every MEM resource a kernel mapping.

   TINY210_QEMU builds the same port for QEMU's Cortex-A8 "cubieboard",
   whose Allwinner A10 peripherals have their own drivers. */
#include "kernel/types.h"
#include "kernel/param.h"
#include "kernel/memlayout.h"
#include "kernel/aarch64.h"
#include "kernel/spinlock.h"
#include "kernel/defs.h"
#include "kernel/device.h"
#include "kernel/drivers/video/fbdev/s3c-fb.h"
#include "kernel/drivers/usb/dwc2/dwc2.h"
#include "kernel/drivers/usb/host/ehci.h"
#include "kernel/drivers/mmc/host/sdhci-s3c.h"
#include "devices.h"

#define MEM(a, sz, n) { (a), (a)+(sz)-1, IORESOURCE_MEM, n }
#define IRQ(i, n)     { (i), (i), IORESOURCE_IRQ, n }

#ifdef TINY210_QEMU
static struct device board_devices[] = {
  { .name = "sun4i-intc",  .resource = { MEM(0x01C20400, 0x400, "intc") }, .nresource = 1 },
  { .name = "sun4i-timer", .resource = { MEM(0x01C20C00, 0x400, "timer"), IRQ(22, "timer0") }, .nresource = 2 },
  { .name = "sun4i-uart",  .resource = { MEM(0x01C28000, 0x400, "uart0") }, .nresource = 1 },
  /* No display in QEMU: plain RAM, so /dev/fb0 and mmap() can be tested. */
  { .name = "simple-framebuffer",
    .resource = { MEM(DRAM_BASE + S5PV210_FB_OFFSET, S5PV210_FB_SIZE, "framebuffer") }, .nresource = 1 },
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

/* USB OTG PHY (Linux arch/arm/mach-s5pv210/setup-usb-phy.c): enable PHY0
   in USB_PHY_CONTROL, 24 MHz reference from XusbXTI, PHY out of power
   down, then pulse PHY/link reset. Runs in the usb-enum thread. */
#define OTG_PHY 0xEC100000U
static void spin_us(uint us) { for(volatile uint i = 0; i < us*100; i++) ; }
static void
otg_phy_init(void)
{
  REG(0xE0100464) |= 1U << 16;                                /* CLK_GATE_IP1: USBOTG */
  REG(0xE010E80C) |= 1;                                       /* USB_PHY_CONTROL: PHY0 */
  REG(OTG_PHY + 0x04) = (REG(OTG_PHY + 0x04) & ~3U) | 3 | (1U << 4);  /* PHYCLK: 24 MHz, CLK_FORCE */
  REG(OTG_PHY + 0x00) &= ~0x19U;                              /* PHYPWR: normal */
  spin_us(1000);
  REG(OTG_PHY + 0x08) = 7;                                    /* RSTCON: PHY, HCLK, PHYCLK */
  spin_us(20);
  REG(OTG_PHY + 0x08) = 0;
  spin_us(1000);
}
static struct dwc2_platdata otg_pdata __attribute__((unused)) = { otg_phy_init };

/* USB host PHY (PHY1, Linux drivers/phy/samsung/phy-s5pv210-usb2.c): host
   isolation off in USB_PHY_CONTROL bit 1, 24 MHz reference, PHY1 out of
   suspend/power-down (UPHYPWR bits 6, 7), reset PHY1 and the host link
   (URSTCON bits 3, 4). Feeds the EHCI/OHCI pair behind the USB-A sockets. */
static void
host_phy_init(void)
{
  REG(0xE0100464) |= 1U << 17;                                /* CLK_GATE_IP1: USBHOST */
  REG(0xE010E80C) |= 1U << 1;                                 /* USB_PHY_CONTROL: PHY1 */
  REG(OTG_PHY + 0x04) = (REG(OTG_PHY + 0x04) & ~3U) | 3;      /* UPHYCLK: 24 MHz */
  REG(OTG_PHY + 0x00) &= ~((1U << 6) | (1U << 7));            /* UPHYPWR: PHY1 on */
  REG(OTG_PHY + 0x08) |= (1U << 3) | (1U << 4);               /* URSTCON: PHY1, host link */
  spin_us(10);
  REG(OTG_PHY + 0x08) &= ~((1U << 3) | (1U << 4));
  spin_us(1000);
}
static struct ehci_platdata ehci_pdata = { host_phy_init };

/* SD card: the HSMMC channel the iROM booted from (BL1 reads the same iROM
   variable); channel 0 if unknown. Pins GPG<n>[6:0] = CLK, CMD, CD, DATA0-3
   (function 2); SCLK_MMC<n> from the 24 MHz crystal (CLK_SRC4 = XXTI,
   ratio 1), clock gate CLK_GATE_IP2 bit 16+n. */
static int sd_channel;
static void
sd_setup(void)
{
  uint32 gpg = 0xE02001A0 + 0x20*sd_channel;
  REG(gpg + 0x0) = (REG(gpg) & ~0x0fffffffU) | 0x02222222;   /* GPGnCON */
  REG(gpg + 0x8) &= ~0x3fffU;                                 /* GPGnPUD: no pulls */
  REG(gpg + 0xc) |= 0x3fffU;                                  /* GPGnDRV: 4x */
  REG(0xE0100468) |= 1U << (16 + sd_channel);                 /* CLK_GATE_IP2: HSMMCn */
  REG(0xE0100210) &= ~(0xfU << (4*sd_channel));               /* CLK_SRC4: MMCn = XXTI */
  REG(0xE0100310) &= ~(0xfU << (4*sd_channel));               /* CLK_DIV4: MMCn ratio 1 */
  REG(0xE0100280) |= 1U << sd_channel;                        /* CLK_SRC_MASK0: MMCn on */
}
static struct sdhci_s3c_platdata sd_pdata = { sd_setup };

static struct device board_devices[] = {
  { .name = "s5pv210-clock", .resource = { MEM(0xE0100000, 0x10000, "clock") }, .nresource = 1 },
  { .name = "s3c2410-wdt",   .resource = { MEM(0xE2700000, 0x400, "wdt") }, .nresource = 1 },
  { .name = "s5pv210-vic",   .resource = { MEM(0xF2000000, 0x400000, "vic0-3") }, .nresource = 1 },
  { .name = "s5pv210-pwm",   .resource = { MEM(0xE2500000, 0x400, "pwm"), IRQ(25, "timer4") }, .nresource = 2 },
  { .name = "s5pv210-uart",  .id = BOARD_UART_N,
    .resource = { MEM(BOARD_UART, 0x400, "uart"), IRQ(42 + BOARD_UART_N, "uart") }, .nresource = 2 },
  { .name = "s5pv210-gpio",  .resource = { MEM(0xE0200000, 0x1000, "gpio") }, .nresource = 1 },
  /* USB-A host sockets: EHCI (high speed; OHCI for full/low speed is not
     driven yet). The OTG port (s3c-hsotg, DWC2 in host mode) is available
     with make S5PV210_USB_OTG=1. */
  { .name = "s3c-sdhci", .driver_data = &sd_pdata,
    .resource = { MEM(0xEB000000, 0x100000, "hsmmc") }, .nresource = 1 },
  { .name = "s5p-ehci", .driver_data = &ehci_pdata,
    .resource = { MEM(0xEC200000, 0x100000, "ehci"), IRQ(55, "uhost"), MEM(OTG_PHY, 0x1000, "usb-phy") },
    .nresource = 3 },
#ifdef S5PV210_USB_OTG
  { .name = "s3c-hsotg", .driver_data = &otg_pdata,
    .resource = { MEM(0xEC000000, 0x100000, "otg"), IRQ(56, "otg"), MEM(OTG_PHY, 0x1000, "otg-phy") },
    .nresource = 3 },
#endif
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
tiny210_map_devices(pagetable_t p)
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

/* Called before the MMU is on with the iROM's boot-device variable
   (0xD0037488: SD/MMC controller base it booted from). */
void
s5pv210_note_boot_device(uint32 irom_sd_base)
{
#ifndef TINY210_QEMU
  if(irom_sd_base >= 0xEB000000U && irom_sd_base <= 0xEB300000U && !(irom_sd_base & 0xFFFFF))
    sd_channel = (irom_sd_base - 0xEB000000U) >> 20;
  for(uint i = 0; i < NDEVICES; i++)
    if(strncmp(board_devices[i].name, "s3c-sdhci", 10) == 0){
      board_devices[i].id = sd_channel;
      board_devices[i].resource[0].start = 0xEB000000U + 0x100000U*sd_channel;
      board_devices[i].resource[0].end = board_devices[i].resource[0].start + 0xFFFFF;
    }
#else
  (void)irom_sd_base;
#endif
}

void
s5pv210_register_devices(void)
{
  for(uint i = 0; i < NDEVICES; i++)
    if(platform_device_register(&board_devices[i]) < 0)
      printf("board: cannot register %s\n", board_devices[i].name);
}
