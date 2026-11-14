// S5PV210 FIMD (display controller) driver, after Linux s3c-fb: one 32 bpp
// window (XRGB8888) scanned out of a frame buffer in DRAM, with a text
// console on top (fbcon.c).
//
// Resources from the board file:
//   [0] MEM  FIMD registers (0xF8000000)
//   [1] MEM  frame buffer, physically contiguous, outside the page
//            allocator, mapped privileged non-cacheable
// driver_data: struct s3c_fb_platdata (panel timings, pin setup).
// Registers follow the S5PV210 user's manual, "Display Controller".
#include "kernel/types.h"
#include "kernel/aarch64.h"
#include "kernel/param.h"
#include "kernel/spinlock.h"
#include "kernel/defs.h"
#include "kernel/device.h"
#include "kernel/module.h"
#include "kernel/clk.h"
#include "s3c-fb.h"
#include "fbcon.h"
#include "fbdev.h"

static struct fb_info s3c_fb_info = { .id = "s3c-fb", .bpp = 32, .has_fbcon = 1 };

#define VIDCON0 0x000
#define VIDCON1 0x004
#define VIDTCON0 0x010
#define VIDTCON1 0x014
#define VIDTCON2 0x018
#define WINCON0 0x020
#define SHADOWCON 0x034
#define VIDOSD0A 0x040
#define VIDOSD0B 0x044
#define VIDOSD0C 0x048
#define VIDW00ADD0B0 0x0A0
#define VIDW00ADD1B0 0x0D0
#define VIDW00ADD2 0x100
static uintptr regs;
#define REG(o) (*(volatile uint32*)(regs + (o)))

static void
color_bars(uint32 *fb, const struct s3c_fb_panel *p)
{
  static const uint32 bar[8] = {0xffffff,0xffff00,0x00ffff,0x00ff00,0xff00ff,0xff0000,0x0000ff,0x000000};
  uint32 w = p->xres/8;
  for(uint32 y = 0; y < p->yres; y++)
    for(uint32 x = 0; x < p->xres; x++)
      fb[y*p->xres + x] = bar[x/w < 8 ? x/w : 7];
}

static int
s3c_fb_probe(struct device *dev)
{
  struct s3c_fb_platdata *pd = dev->driver_data;
  const struct s3c_fb_panel *p = pd->panel;
  regs = (uintptr)dev->resource[0].start;
  uint32 fbpa = (uint32)dev->resource[1].start;
  uint32 fbsize = p->xres*p->yres*4;
  if(fbsize > dev->resource[1].end - dev->resource[1].start + 1){
    printf("s3c-fb: %s frame buffer does not fit\n", p->name);
    return -1;
  }
  if(pd->setup_gpio)
    pd->setup_gpio();

  // Pixel clock: HCLK_DSYS / (CLKVAL+1), at most what the panel needs.
  uint32 hclk = clk_get_rate("hclk_dsys");
  if(hclk == 0) hclk = 166750000;
  uint32 htot = p->xres+p->hfp+p->hbp+p->hsw, vtot = p->yres+p->vfp+p->vbp+p->vsw;
  uint32 want = htot*vtot*p->hz;
  uint32 clkval = (hclk+want-1)/want - 1;
  if(clkval > 255) clkval = 255;

  REG(VIDCON0) = 0;                             // off while programming
  REG(VIDCON1) = (p->ivclk<<7)|(p->ihsync<<6)|(p->ivsync<<5)|(p->ivden<<4);
  REG(VIDTCON0) = ((p->vbp-1)<<16)|((p->vfp-1)<<8)|(p->vsw-1);
  REG(VIDTCON1) = ((p->hbp-1)<<16)|((p->hfp-1)<<8)|(p->hsw-1);
  REG(VIDTCON2) = ((p->yres-1)<<11)|(p->xres-1);
  REG(WINCON0) = (0xBU<<2);                     // BPPMODE_F: XRGB8888
  REG(VIDOSD0A) = 0;
  REG(VIDOSD0B) = ((p->xres-1)<<11)|(p->yres-1);
  REG(VIDOSD0C) = p->xres*p->yres;              // window size in words
  REG(VIDW00ADD0B0) = fbpa;
  REG(VIDW00ADD1B0) = fbpa + fbsize;
  REG(VIDW00ADD2) = p->xres*4;                  // OFFSIZE 0, PAGEWIDTH bytes
  REG(SHADOWCON) |= 1;                          // window 0 channel on
  REG(WINCON0) |= 1;                            // ENWIN_F
  REG(VIDCON0) = (clkval<<6)|(1<<4)|3;          // CLKVAL, divided, ENVID(_F)

  uint32 *fb = (uint32*)(uintptr)fbpa;
  color_bars(fb, p);
  uint32 pclk = hclk/(clkval+1);
  printf("s3c-fb: %s %dx%d, pixel clock %d Hz (%d Hz refresh), fb %x\n",
         p->name, p->xres, p->yres, pclk, pclk/(htot*vtot), fbpa);
  for(volatile uint32 i = 0; i < 20000000; i++) ;   // show the bars a moment
  fbcon_register(fb, p->xres, p->yres);
  s3c_fb_info.phys = fbpa;
  s3c_fb_info.size = dev->resource[1].end - dev->resource[1].start + 1;
  s3c_fb_info.screen = fb;
  s3c_fb_info.xres = p->xres;
  s3c_fb_info.yres = p->yres;
  s3c_fb_info.line_length = p->xres*4;
  return register_framebuffer(&s3c_fb_info);
}

static struct device_driver s3c_fb_driver = { .name = "s3c-fb", .bus = &platform_bus, .probe = s3c_fb_probe };
static void s3c_fb_init(void) { platform_driver_register(&s3c_fb_driver); }
module_init_ord("s3c-fb", s3c_fb_init, 1);
