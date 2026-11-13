#ifndef XV6_WIN32K_H
#define XV6_WIN32K_H

#include <kernel/types.h>

// The frame buffer as the kernel sees it (bcm2708_fb.c).
struct fb_kinfo {
  uint32 *pixels;               // kernel mapping, Normal non-cacheable
  int width, height;
  int stride;                   // pixels per line
  int rgb;                      // firmware pixel order: 1 = RGB, 0 = BGR
};
int fb_kernel_info(struct fb_kinfo *ki);

// Window manager (drivers/video/win32k.c).
void win32k_input_event(uint16 type, uint16 code, int value);
void win32k_exit(int tgid);
int  win32k_owns_input(void);   // a session is up: keys are not for the tty

#endif
