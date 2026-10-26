#ifndef _UAPI_FB_H
#define _UAPI_FB_H

#include "types.h"

/* Subset of Linux include/uapi/linux/fb.h: same ioctl numbers and the
 * leading fields of the two screeninfo structures. */
#define FBIOGET_VSCREENINFO  0x4600
#define FBIOGET_FSCREENINFO  0x4602

struct fb_fix_screeninfo {
  char id[16];
  uint64 smem_start;      /* physical start of the frame buffer */
  uint32 smem_len;        /* length of the frame buffer: mmap() this */
  uint32 line_length;     /* bytes per scan line (pitch) */
};

struct fb_var_screeninfo {
  uint32 xres;
  uint32 yres;
  uint32 xres_virtual;
  uint32 yres_virtual;
  uint32 bits_per_pixel;
};

#endif
