// "simple-framebuffer": a fixed, already scanned-out (or, under QEMU, just
// reserved) buffer described entirely by its board resource, 640x480
// XRGB8888. The X210_QEMU stand-in build uses it so that /dev/fb0, its
// ioctls and mmap() are exercised by the automatic test.
#include "kernel/types.h"
#include "kernel/aarch64.h"
#include "kernel/param.h"
#include "kernel/spinlock.h"
#include "kernel/defs.h"
#include "kernel/device.h"
#include "kernel/module.h"
#include "fbdev.h"

static struct fb_info simplefb_info = { .id = "simplefb", .bpp = 32, .xres = 640, .yres = 480, .line_length = 640*4 };

static int
simplefb_probe(struct device *dev)
{
  simplefb_info.phys = dev->resource[0].start;
  simplefb_info.size = dev->resource[0].end - dev->resource[0].start + 1;
  simplefb_info.screen = (uint32*)(uintptr)dev->resource[0].start;
  if(simplefb_info.line_length * simplefb_info.yres > simplefb_info.size)
    return -1;
  memset(simplefb_info.screen, 0, simplefb_info.line_length * simplefb_info.yres);
  return register_framebuffer(&simplefb_info);
}

static struct device_driver simplefb_driver = { .name = "simple-framebuffer", .bus = &platform_bus, .probe = simplefb_probe };
static void simplefb_init(void) { platform_driver_register(&simplefb_driver); }
module_init_ord("simplefb", simplefb_init, 1);
