// Frame buffer text console (a tiny Linux fbcon): fixed 8x16 font, light
// grey on dark blue, scrolls by moving the frame buffer up one text row.
#include "kernel/types.h"
#include "kernel/aarch64.h"
#include "kernel/defs.h"
#include "fbcon.h"
#include "font8x16.h"

static uint32 *fb;
static uint32 xres, yres, cols, rows, cx, cy;
static int paused;
static const uint32 fg = 0x00e0e0e0, bg = 0x00102040;

#ifndef FBCON_FONT_BOLD
#define FBCON_FONT_BOLD 1
#endif

/* The original 8x16 bitmap contains many one-pixel stems.  On the 7-inch
 * RGB panel they are hard to read even when the bus timing is correct.
 * Widen them by one pixel, but do not add pixels in the two edge columns:
 * adjacent character cells therefore never join together. */
static uchar
font_row(const uchar *g, int y)
{
  uchar bits = g ? g[y] : 0;
#if FBCON_FONT_BOLD
  bits |= (bits >> 1) & 0x7e;
#endif
  return bits;
}

static void
fill(uint32 x, uint32 y, uint32 w, uint32 h, uint32 c)
{
  for(uint32 j = y; j < y+h; j++){
    uint32 *p = fb + j*xres + x;
    for(uint32 i = 0; i < w; i++) p[i] = c;
  }
}

static void
glyph(uint32 col, uint32 row, int ch)
{
  uint32 *p = fb + (row*FONT_H)*xres + col*FONT_W;
  const uchar *g = (ch >= FONT_FIRST && ch <= FONT_LAST) ? font8x16[ch-FONT_FIRST] : 0;
  for(int y = 0; y < FONT_H; y++, p += xres){
    uchar bits = font_row(g, y);
    for(int x = 0; x < FONT_W; x++) p[x] = (bits & (0x80>>x)) ? fg : bg;
  }
}

void
fbcon_register(uint32 *buf, uint32 w, uint32 h)
{
  fb = buf; xres = w; yres = h;
  cols = w/FONT_W; rows = h/FONT_H; cx = cy = 0;
  fill(0, 0, w, h, bg);
  printf("fbcon: %dx%d text console, 8x16 font%s\n", cols, rows,
         FBCON_FONT_BOLD ? " bold" : "");
}

void
fbcon_pause(int pause)
{
  paused = pause;
  if(!pause && fb){
    fill(0, 0, xres, yres, bg);
    cx = cy = 0;
  }
}

void
fbcon_putc(int c)
{
  if(!fb || paused) return;
  if(c == '\n') { cx = 0; cy++; }
  else if(c == '\r') cx = 0;
  else if(c == '\b' || c == 0x7f) { if(cx) { cx--; glyph(cx, cy, ' '); } }
  else if(c == '\t') { do { glyph(cx, cy, ' '); cx++; } while(cx%8 && cx < cols); }
  else if(c >= ' ') { glyph(cx, cy, c); cx++; }
  if(cx >= cols) { cx = 0; cy++; }
  if(cy >= rows){
    uint32 line = xres*FONT_H;
    memmove(fb, fb+line, (rows-1)*line*4);
    fill(0, (rows-1)*FONT_H, xres, FONT_H, bg);
    cy = rows-1;
  }
}
