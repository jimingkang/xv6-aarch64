// Frame buffer core: /dev/fb0 for the one registered framebuffer.
//   ioctl(FBIOGET_VSCREENINFO / FBIOGET_FSCREENINFO): geometry and the
//     XRGB8888 channel layout, as Linux reports them
//   mmap(MAP_SHARED, offset 0): the buffer itself, user read/write,
//     non-cacheable, shared across fork
// While any process has /dev/fb0 open the text console (fbcon) stops
// drawing, so it does not scribble over the application's picture; the
// last close gives the screen back to fbcon.
#include "kernel/types.h"
#include "kernel/aarch64.h"
#include "kernel/param.h"
#include "kernel/spinlock.h"
#include "kernel/sleeplock.h"
#include "kernel/fs.h"
#include "kernel/file.h"
#include "kernel/proc.h"
#include "kernel/defs.h"
#include "kernel/device.h"
#include "kernel/mmap.h"
#include "fbdev.h"
#include "fbcon.h"

static struct fb_info *fb;
static struct spinlock fb_lock;
static int fb_users;

void fbcon_pause(int pause) __attribute__((weak));

static int
fb_open(struct file *f)
{
  if(fb == 0)
    return -1;
  acquire(&fb_lock);
  int first = fb_users++ == 0;
  release(&fb_lock);
  if(first && fb->has_fbcon && fbcon_pause)
    fbcon_pause(1);
  return 0;
}

static void
fb_release(struct file *f)
{
  acquire(&fb_lock);
  int last = --fb_users == 0;
  release(&fb_lock);
  if(last && fb->has_fbcon && fbcon_pause)
    fbcon_pause(0);
}

static int
fb_ioctl(struct file *f, uint64 cmd, uint64 arg)
{
  struct proc *p = myproc();
  if(cmd == FBIOGET_FSCREENINFO){
    struct fb_fix_screeninfo fix;
    memset(&fix, 0, sizeof(fix));
    safestrcpy(fix.id, fb->id, sizeof(fix.id));
    fix.smem_start = fb->phys;
    fix.smem_len = fb->line_length * fb->yres;
    fix.line_length = fb->line_length;
    return copyout(p->vm->pagetable, arg, (char*)&fix, sizeof(fix));
  }
  if(cmd == FBIOGET_VSCREENINFO){
    struct fb_var_screeninfo var;
    memset(&var, 0, sizeof(var));
    var.xres = var.xres_virtual = fb->xres;
    var.yres = var.yres_virtual = fb->yres;
    var.bits_per_pixel = fb->bpp;
    var.red.offset = 16;   var.red.length = 8;     // XRGB8888: 0x00RRGGBB
    var.green.offset = 8;  var.green.length = 8;
    var.blue.offset = 0;   var.blue.length = 8;
    return copyout(p->vm->pagetable, arg, (char*)&var, sizeof(var));
  }
  return -1;   // Linux: -ENOTTY
}

static int
fb_mmap(struct file *f, struct vm_area_struct *vma)
{
  uint64 len = vma->vm_end - vma->vm_start;
  if(vma->vm_pgoff != 0 || len > ((fb->size + MMAP_ALIGN - 1) & ~(MMAP_ALIGN - 1)))
    return -1;
  return remap_pfn_range(vma, vma->vm_start, fb->phys, len, vma->vm_prot);
}

static struct file_operations fb_fops = {
  .open = fb_open,
  .release = fb_release,
  .ioctl = fb_ioctl,
  .mmap = fb_mmap,
};

int
register_framebuffer(struct fb_info *info)
{
  if(fb != 0 || (info->phys & (MMAP_ALIGN - 1)))
    return -1;
  initlock(&fb_lock, "fb");
  fb = info;
  if(register_chrdev(FB, "fb0", &fb_fops) < 0){
    fb = 0;
    return -1;
  }
  printf("fb0: %s %dx%d %d bpp, %d bytes at %x\n", info->id, info->xres,
         info->yres, info->bpp, info->line_length * info->yres, (uint)info->phys);
  return 0;
}
