#ifndef XV6_FB_H
#define XV6_FB_H

// Needs kernel/types.h.

/* Subset of Linux include/uapi/linux/fb.h: same ioctl numbers and the
 * leading fields of the two screeninfo structures, in Linux order. */
#define FBIOGET_VSCREENINFO  0x4600
#define FBIOGET_FSCREENINFO  0x4602

struct fb_fix_screeninfo {
  char id[16];
  uint64 smem_start;      /* physical start of the frame buffer */
  uint32 smem_len;        /* length of the frame buffer: mmap() this */
  uint32 line_length;     /* bytes per scan line (pitch) */
};

/* Where a colour channel sits inside one pixel (Linux struct fb_bitfield). */
struct fb_bitfield {
  uint32 offset;          /* bit position of the channel's LSB */
  uint32 length;          /* channel width in bits */
  uint32 msb_right;
};

struct fb_var_screeninfo {
  uint32 xres;
  uint32 yres;
  uint32 xres_virtual;
  uint32 yres_virtual;
  uint32 xoffset;
  uint32 yoffset;
  uint32 bits_per_pixel;
  uint32 grayscale;
  struct fb_bitfield red;   /* 32 bpp: red.offset 16 means 0x00RRGGBB */
  struct fb_bitfield green;
  struct fb_bitfield blue;
  struct fb_bitfield transp;
};

#endif
