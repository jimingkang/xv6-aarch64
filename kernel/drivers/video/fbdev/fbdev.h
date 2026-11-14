#ifndef XV6_FBDEV_H
#define XV6_FBDEV_H
// Frame buffer core (Linux fbmem.c): a driver describes its scan-out buffer
// in a struct fb_info and calls register_framebuffer(); the core provides
// /dev/fb0 (major FB) with FBIOGET_[VF]SCREENINFO and mmap().
#include "kernel/fb.h"

struct fb_info {
  char id[16];
  uint64 phys;            // physical start, 1 MiB aligned (mmap uses sections)
  uint32 size;            // bytes reserved for the buffer (>= line_length*yres)
  uint32 *screen;         // kernel address of the buffer
  uint32 xres, yres, bpp, line_length;
  int has_fbcon;          // the text console draws here when nobody has it open
};

int register_framebuffer(struct fb_info *info);
#endif
