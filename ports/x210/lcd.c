/* S5PV210 FIMD (display controller) driver for the FriendlyARM Tiny210 /
   九鼎 X210 RGB LCD: one 32 bpp window (XRGB8888) scanned out of a
   frame buffer in DRAM, plus a text console in 8x16 glyphs that mirrors
   the kernel console.

   The frame buffer sits just above the memory handed to the page allocator
   (DRAM_BASE + USE_DRAM), so it is physically contiguous and never
   allocated. It is mapped privileged, non-cacheable (caches are off in this
   bring-up anyway).

   Panel timings come from the panel table below and are chosen at build
   time (make S5PV210_LCD=s70|h43|w50|a70). Registers follow the S5PV210
   user's manual, chapter "Display Controller", and Linux s3c-fb. */
#include "kernel/types.h"
#include "kernel/param.h"
#include "kernel/memlayout.h"
#include "kernel/aarch64.h"
#include "kernel/spinlock.h"
#include "kernel/defs.h"
#include "board.h"
#include "lcd.h"
#include "font8x16.h"

#define REG(a) (*(volatile uint32*)(uintptr)(a))

/* FIMD */
#define FIMD 0xF8000000U
#define VIDCON0 (FIMD+0x000)
#define VIDCON1 (FIMD+0x004)
#define VIDTCON0 (FIMD+0x010)
#define VIDTCON1 (FIMD+0x014)
#define VIDTCON2 (FIMD+0x018)
#define WINCON0 (FIMD+0x020)
#define SHADOWCON (FIMD+0x034)
#define VIDOSD0A (FIMD+0x040)
#define VIDOSD0B (FIMD+0x044)
#define VIDOSD0C (FIMD+0x048)
#define VIDW00ADD0B0 (FIMD+0x0A0)
#define VIDW00ADD1B0 (FIMD+0x0D0)
#define VIDW00ADD2 (FIMD+0x100)
/* System and GPIO */
#define CLK_GATE_IP1 0xE0100464U
#define DISPLAY_CONTROL 0xE0107008U   /* [1:0] = 2: RGB interface via FIMD */
#define GPF_CON(n) (0xE0200120U+0x20*(n))
#define GPF_DRV(n) (0xE020012CU+0x20*(n))
/* HCLK_DSYS feeds FIMD (VIDCON0.CLKSEL_F = 0): MPLL 667 MHz / 4. */
#define HCLK_DSYS 166750000U

struct panel {
  const char *name;
  uint32 xres, yres, hz;
  uint32 hfp, hbp, hsw, vfp, vbp, vsw;
  int ivclk, ihsync, ivsync, ivden;  /* VIDCON1 polarity bits */
};
/* FriendlyARM panels (mini210/tiny210 BSP timings). The DE-mode panels
   mostly ignore sync widths; totals and pixel clock matter. */
static const struct panel panels[] = {
  {"S70", 800, 480, 60,  80, 36, 10, 22, 15, 8, 1, 1, 1, 0},
  {"H43", 480, 272, 60,   5, 40,  2,  8,  8, 2, 1, 1, 1, 0},
  {"W50", 800, 480, 60,  40, 40, 48, 20, 20, 12, 1, 1, 1, 0},
  {"A70", 800, 480, 60,  40, 40, 48, 17, 29, 24, 1, 1, 1, 0},
};
#ifndef S5PV210_LCD_INDEX
#define S5PV210_LCD_INDEX 0
#endif
static const struct panel *pn = &panels[S5PV210_LCD_INDEX];

static uint32 *fb;
static uint32 fb_size;
static int lcd_ready;
static uint32 cols, rows, cx, cy;
static const uint32 fg = 0x00e0e0e0, bg = 0x00102040;

uint32 lcd_fb_phys(void) { return DRAM_BASE + LCD_FB_OFFSET; }

static void fill(uint32 x, uint32 y, uint32 w, uint32 h, uint32 c) {
  for(uint32 j=y;j<y+h;j++) {
    uint32 *p=fb+j*pn->xres+x;
    for(uint32 i=0;i<w;i++) p[i]=c;
  }
}

static void glyph(uint32 col, uint32 row, int ch) {
  uint32 *p=fb+(row*FONT_H)*pn->xres+col*FONT_W;
  const uchar *g = (ch>=FONT_FIRST && ch<=FONT_LAST) ? font8x16[ch-FONT_FIRST] : 0;
  for(int y=0;y<FONT_H;y++,p+=pn->xres) {
    uchar bits = g ? g[y] : 0;
    for(int x=0;x<FONT_W;x++) p[x] = (bits & (0x80>>x)) ? fg : bg;
  }
}

static void scroll(void) {
  uint32 line=pn->xres*FONT_H;
  memmove(fb, fb+line, (rows-1)*line*4);
  fill(0,(rows-1)*FONT_H,pn->xres,FONT_H,bg);
}

void lcd_putc(int c) {
  if(!lcd_ready) return;
  if(c=='\n') { cx=0; cy++; }
  else if(c=='\r') cx=0;
  else if(c=='\b' || c==0x7f) { if(cx) { cx--; glyph(cx,cy,' '); } }
  else if(c=='\t') { do { glyph(cx,cy,' '); cx++; } while(cx%8 && cx<cols); }
  else if(c>=' ') { glyph(cx,cy,c); cx++; }
  if(cx>=cols) { cx=0; cy++; }
  if(cy>=rows) { scroll(); cy=rows-1; }
}

static void color_bars(void) {
  static const uint32 bar[8]={0xffffff,0xffff00,0x00ffff,0x00ff00,0xff00ff,0xff0000,0x0000ff,0x000000};
  uint32 w=pn->xres/8;
  for(int i=0;i<8;i++) fill(i*w,0,i==7 ? pn->xres-7*w : w,pn->yres,bar[i]);
}

void lcd_init(void) {
  const struct panel *p=pn;
  fb=(uint32*)(uintptr)lcd_fb_phys();
  fb_size=p->xres*p->yres*4;
  if(fb_size>LCD_FB_SIZE) { printf("lcd: %s frame buffer too large\n",p->name); return; }

  /* Pins: GPF0[7:0], GPF1[7:0], GPF2[7:0], GPF3[3:0] = LCD (function 2),
     strongest drive (as Linux s5pv210 fb setup does). */
  for(int n=0;n<3;n++) { REG(GPF_CON(n))=0x22222222; REG(GPF_DRV(n))=0xffffffff; }
  REG(GPF_CON(3))=(REG(GPF_CON(3))&~0xffffU)|0x2222;
  REG(GPF_DRV(3))|=0xff;
  REG(CLK_GATE_IP1)|=1;                       /* FIMD clock on */
  REG(DISPLAY_CONTROL)=(REG(DISPLAY_CONTROL)&~3U)|2;

  /* Pixel clock: HCLK_DSYS / (CLKVAL+1) >= the panel's need. */
  uint32 htot=p->xres+p->hfp+p->hbp+p->hsw, vtot=p->yres+p->vfp+p->vbp+p->vsw;
  uint32 want=htot*vtot*p->hz;
  uint32 clkval=(HCLK_DSYS+want-1)/want-1;    /* pixel clock <= want */
  if(clkval>255) clkval=255;

  REG(VIDCON0)=0;                             /* off while programming */
  REG(VIDCON1)=(p->ivclk<<7)|(p->ihsync<<6)|(p->ivsync<<5)|(p->ivden<<4);
  REG(VIDTCON0)=((p->vbp-1)<<16)|((p->vfp-1)<<8)|(p->vsw-1);
  REG(VIDTCON1)=((p->hbp-1)<<16)|((p->hfp-1)<<8)|(p->hsw-1);
  REG(VIDTCON2)=((p->yres-1)<<11)|(p->xres-1);
  REG(WINCON0)=(0xBU<<2);                     /* BPPMODE_F: unpacked 24 bpp (XRGB8888) */
  REG(VIDOSD0A)=0;
  REG(VIDOSD0B)=((p->xres-1)<<11)|(p->yres-1);
  REG(VIDOSD0C)=p->xres*p->yres;              /* window size in words */
  REG(VIDW00ADD0B0)=lcd_fb_phys();
  REG(VIDW00ADD1B0)=lcd_fb_phys()+fb_size;
  REG(VIDW00ADD2)=p->xres*4;                  /* OFFSIZE 0, PAGEWIDTH in bytes */
  REG(SHADOWCON)|=1;                          /* window 0 channel on */
  REG(WINCON0)|=1;                            /* ENWIN_F */
  REG(VIDCON0)=(clkval<<6)|(1<<4)|3;          /* CLKVAL, divided clock, ENVID, ENVID_F */

  color_bars();
  uint32 pclk=HCLK_DSYS/(clkval+1);
  printf("lcd: FIMD %s %dx%d, pixel clock %d Hz (%d Hz refresh), fb %x\n",
         p->name,p->xres,p->yres,pclk,pclk/(htot*vtot),lcd_fb_phys());
  for(volatile uint32 i=0;i<20000000;i++) ;   /* show the bars for a moment */
  cols=p->xres/FONT_W; rows=p->yres/FONT_H; cx=cy=0;
  fill(0,0,p->xres,p->yres,bg);
  lcd_ready=1;
}
