// /dev/fb0: the VideoCore firmware framebuffer, after Linux
// drivers/video/fbdev/bcm2708_fb.c.
//
// The firmware allocates the frame buffer in GPU memory at the top of RAM
// (on QEMU raspi3b too), outside the kernel's direct map.  Userspace draws
// into it through mmap(): f_op->mmap maps the frames write-combining
// (Normal non-cacheable, Linux pgprot_writecombine) with remap_pfn_range().
// ioctl(FBIOGET_[VF]SCREENINFO) reports the geometry.

#include <kernel/types.h>
#include <kernel/param.h>
#include "memlayout.h"
#include "aarch64.h"
#include <kernel/spinlock.h>
#include <kernel/sleeplock.h>
#include <kernel/fs.h>
#include <kernel/file.h>
#include <kernel/defs.h>
#include <kernel/device.h>
#include <kernel/module.h>
#include <kernel/mbox.h>
#include <kernel/mm.h>
#include <uapi/fb.h>

#define FB_WIDTH   640
#define FB_HEIGHT  480
#define FB_DEPTH   32

static struct {
  struct spinlock lock;
  int ready;
  uint32 width, height, depth, pitch;
  uint64 base;                  // ARM physical address
  uint32 size;
} fb;

// Configure and allocate in one property message; the firmware ignores a
// separate ALLOCATE_BUFFER that follows the geometry in another message.
static int
fb_alloc(void)
{
  uint32 t[] = {
    MBOX_TAG_SET_PHYS_WH,     8, 0, FB_WIDTH, FB_HEIGHT,
    MBOX_TAG_SET_VIRT_WH,     8, 0, FB_WIDTH, FB_HEIGHT,
    MBOX_TAG_SET_VIRT_OFFSET, 8, 0, 0, 0,
    MBOX_TAG_SET_DEPTH,       4, 0, FB_DEPTH,
    MBOX_TAG_SET_PIXEL_ORDER, 4, 0, 1,          // RGB
    MBOX_TAG_ALLOCATE_BUFFER, 8, 0, PGSIZE, 0,
    MBOX_TAG_GET_PITCH,       4, 0, 0,
  };

  if(mbox_property_list(t, NELEM(t)) < 0 || t[26] == 0 || t[27] == 0)
    return -1;
  fb.width = t[3];
  fb.height = t[4];
  fb.depth = t[18];
  fb.base = t[26] & 0x3fffffffU;             // VC bus address -> ARM PA
  fb.size = t[27];
  fb.pitch = t[31];
  return 0;
}

static int
fb_open(struct file *f)
{
  int r = 0;

  acquire(&fb.lock);
  if(!fb.ready){
    if(fb_alloc() == 0){
      fb.ready = 1;
      printf("fb0: %dx%d %d bpp pitch %d, %d bytes at pa %p\n", fb.width,
             fb.height, fb.depth, fb.pitch, fb.size, fb.base);
    } else {
      printf("fb0: firmware did not allocate a frame buffer\n");
      r = -1;
    }
  }
  release(&fb.lock);
  return r;
}

static int
fb_mmap(struct file *f, struct vm_area_struct *vma)
{
  uint64 size = vma->vm_end - vma->vm_start;
  uint64 len = PGROUNDUP(fb.size);

  if(!fb.ready || vma->vm_pgoff > len / PGSIZE ||
     size > len - vma->vm_pgoff * PGSIZE || (vma->vm_flags & VM_EXEC))
    return -1;
  return remap_pfn_range(vma, vma->vm_start,
                         (fb.base >> PGSHIFT) + vma->vm_pgoff, size,
                         PTE_NORMAL_NC);
}

static int
fb_ioctl(struct file *f, uint64 cmd, uint64 arg)
{
  if(!fb.ready)
    return -1;
  if(cmd == FBIOGET_FSCREENINFO){
    struct fb_fix_screeninfo fix;
    memset(&fix, 0, sizeof(fix));
    safestrcpy(fix.id, "BCM2708 FB", sizeof(fix.id));
    fix.smem_start = fb.base;
    fix.smem_len = fb.size;
    fix.line_length = fb.pitch;
    return either_copyout(1, arg, &fix, sizeof(fix));
  }
  if(cmd == FBIOGET_VSCREENINFO){
    struct fb_var_screeninfo var;
    memset(&var, 0, sizeof(var));
    var.xres = var.xres_virtual = fb.width;
    var.yres = var.yres_virtual = fb.height;
    var.bits_per_pixel = fb.depth;
    return either_copyout(1, arg, &var, sizeof(var));
  }
  return -1;
}

static void
bcm2708_fb_init(void)
{
  static struct file_operations fops = {
    .open = fb_open,
    .ioctl = fb_ioctl,
    .mmap = fb_mmap,
  };
  static struct device fb_device = {
    .name = "fb0",
    .class = "graphics",
    .devnode = "fb0",
    .dev_major = FB,
  };

  initlock(&fb.lock, "fb0");
  if(register_chrdev(FB, "fb0", &fops) < 0 || device_register(&fb_device) < 0)
    printf("fb0: cannot register /dev/fb0\n");
}

module_init("bcm2708_fb", bcm2708_fb_init);
