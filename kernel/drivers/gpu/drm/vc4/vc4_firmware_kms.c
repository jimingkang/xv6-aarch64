// VC4 DRM/KMS object model and userspace ABI.
//
// Stage two prefers the native HVS/Pixel Valve backend.  The firmware backend
// remains available for QEMU (which does not model those blocks) and for a Pi
// whose HDMI pipeline was not active when the driver probed.

#include <kernel/types.h>
#include <kernel/param.h>
#include "memlayout.h"
#include "aarch64.h"
#include <kernel/spinlock.h>
#include <kernel/mutex.h>
#include <kernel/fs.h>
#include <kernel/file.h>
#include <kernel/proc.h>
#include <kernel/defs.h>
#include <kernel/device.h>
#include <kernel/drm.h>
#include <kernel/bcm2708_fb.h>
#include <kernel/vc4_native.h>
#include <kernel/mm.h>
#include <kernel/module.h>
#include <uapi/device.h>
#include <uapi/mman.h>
#include <uapi/poll.h>

#define VC4_CRTC_ID       1U
#define VC4_CONNECTOR_ID  2U
#define VC4_ENCODER_ID    3U
#define VC4_FB_ID_BASE    0x100U

struct vc4_firmware_kms {
  struct spinlock lock;
  // Serializes sleeping firmware commits and the software scanout state.
  struct mutex commit_lock;
  struct drm_device drm;
  struct bcm2708_fb_info scanout;
  struct drm_file *master;
  uint32 current_fb;
  int native;
};

static struct vc4_firmware_kms vc4;

static uint64
vc4_now_us(void)
{
  uint64 count = r_cntvct_el0();
  uint64 freq = r_cntfrq_el0();

  return (count / freq) * 1000000ULL +
         (count % freq) * 1000000ULL / freq;
}

static void
vc4_fill_mode(struct drm_mode_modeinfo *mode,
              const struct bcm2708_fb_info *info)
{
  memset(mode, 0, sizeof(*mode));
  mode->clock = 25175;
  mode->hdisplay = info->width;
  mode->hsync_start = 656;
  mode->hsync_end = 752;
  mode->htotal = 800;
  mode->vdisplay = info->height;
  mode->vsync_start = 490;
  mode->vsync_end = 492;
  mode->vtotal = 525;
  mode->vrefresh = 60;
  safestrcpy(mode->name, "640x480", sizeof(mode->name));
}

static struct drm_gem_object *
vc4_object_by_handle(struct drm_file *file, uint32 handle)
{
  for(uint32 i = 0; i < file->object_count; i++)
    if(file->objects[i].handle == handle)
      return &file->objects[i];
  return 0;
}

static struct drm_gem_object *
vc4_object_by_offset(struct drm_file *file, uint64 offset)
{
  for(uint32 i = 0; i < file->object_count; i++)
    if(file->objects[i].mmap_offset == offset)
      return &file->objects[i];
  return 0;
}

static struct drm_framebuffer *
vc4_fb_by_id(struct drm_file *file, uint32 id)
{
  for(uint32 i = 0; i < DRM_MAX_GEM_OBJECTS; i++)
    if(file->framebuffers[i].id == id)
      return &file->framebuffers[i];
  return 0;
}

static int
vc4_open(struct drm_device *dev, struct file *f)
{
  struct vc4_firmware_kms *kms = dev->dev_private;
  struct drm_file *file;

  file = kmalloc(sizeof(*file));
  if(file == 0)
    return -1;
  memset(file, 0, sizeof(*file));
  file->dev = dev;
  acquire(&kms->lock);
  // KMS has one scanout owner.  fbdev remains available for the
  // boot console, but two independent DRM masters cannot race page flips.
  if(kms->master){
    release(&kms->lock);
    kfree(file);
    return -1;
  }
  kms->master = file;
  release(&kms->lock);
  f->private_data = file;
  return 0;
}

static void
vc4_postclose(struct drm_device *dev, struct file *f)
{
  struct vc4_firmware_kms *kms = dev->dev_private;
  struct drm_file *file = f->private_data;

  if(file == 0)
    return;
  mutex_lock(&kms->commit_lock);
  // Leave the console on a stable buffer and never retain an object id owned
  // by a drm_file that is about to disappear.
  if(kms->native)
    vc4_native_page_flip(0, 0);
  else
    bcm2708_fb_page_flip(0, 0);
  acquire(&kms->lock);
  if(kms->master == file){
    kms->master = 0;
    kms->current_fb = 0;
  }
  release(&kms->lock);
  mutex_unlock(&kms->commit_lock);
  kfree(file);
}

static int
vc4_version(struct drm_device *dev, uint64 arg)
{
  struct drm_version v;

  memset(&v, 0, sizeof(v));
  v.version_major = 1;
  v.version_minor = 0;
  safestrcpy(v.name, dev->driver->name, sizeof(v.name));
  safestrcpy(v.desc, dev->driver->desc, sizeof(v.desc));
  return either_copyout(1, arg, &v, sizeof(v));
}

static int
vc4_get_resources(struct vc4_firmware_kms *kms, uint64 arg)
{
  struct drm_mode_card_res r;
  uint32 current_fb;

  acquire(&kms->lock);
  current_fb = kms->current_fb;
  release(&kms->lock);
  memset(&r, 0, sizeof(r));
  r.count_fbs = current_fb ? 1 : 0;
  r.count_crtcs = r.count_connectors = r.count_encoders = 1;
  r.fb_id = current_fb;
  r.crtc_id = VC4_CRTC_ID;
  r.connector_id = VC4_CONNECTOR_ID;
  r.encoder_id = VC4_ENCODER_ID;
  r.min_width = r.max_width = kms->scanout.width;
  r.min_height = r.max_height = kms->scanout.height;
  return either_copyout(1, arg, &r, sizeof(r));
}

static int
vc4_get_connector(struct vc4_firmware_kms *kms, uint64 arg)
{
  struct drm_mode_get_connector c;

  if(either_copyin(&c, 1, arg, sizeof(c)) < 0 ||
     c.connector_id != VC4_CONNECTOR_ID)
    return -1;
  memset(&c, 0, sizeof(c));
  c.connector_id = VC4_CONNECTOR_ID;
  c.encoder_id = VC4_ENCODER_ID;
  c.connector_type = DRM_MODE_CONNECTOR_HDMIA;
  c.connection = DRM_MODE_CONNECTED;
  c.count_modes = 1;
  vc4_fill_mode(&c.mode, &kms->scanout);
  return either_copyout(1, arg, &c, sizeof(c));
}

static int
vc4_get_crtc(struct vc4_firmware_kms *kms, uint64 arg)
{
  struct drm_mode_crtc c;
  uint32 current_fb;

  if(either_copyin(&c, 1, arg, sizeof(c)) < 0 || c.crtc_id != VC4_CRTC_ID)
    return -1;
  memset(&c, 0, sizeof(c));
  acquire(&kms->lock);
  current_fb = kms->current_fb;
  release(&kms->lock);
  c.crtc_id = VC4_CRTC_ID;
  c.fb_id = current_fb;
  c.mode_valid = 1;
  vc4_fill_mode(&c.mode, &kms->scanout);
  return either_copyout(1, arg, &c, sizeof(c));
}

static int
vc4_create_dumb(struct vc4_firmware_kms *kms, struct drm_file *file,
                uint64 arg)
{
  struct drm_mode_create_dumb d;
  struct drm_gem_object *obj;
  uint32 slot;
  uint64 span = PGROUNDUP(kms->scanout.frame_bytes);

  if(either_copyin(&d, 1, arg, sizeof(d)) < 0 ||
     d.width != kms->scanout.width || d.height != kms->scanout.height ||
     d.bpp != kms->scanout.depth ||
     file->object_count >= kms->scanout.buffer_count)
    return -1;
  slot = file->object_count;
  obj = &file->objects[file->object_count++];
  memset(obj, 0, sizeof(*obj));
  obj->handle = slot + 1;
  obj->slot = slot;
  obj->paddr = kms->scanout.base + slot * kms->scanout.frame_bytes;
  obj->size = span;
  obj->mmap_offset = slot * span;
  obj->refcount = 1;
  d.handle = obj->handle;
  d.pitch = kms->scanout.pitch;
  d.size = span;
  return either_copyout(1, arg, &d, sizeof(d));
}

static int
vc4_map_dumb(struct drm_file *file, uint64 arg)
{
  struct drm_mode_map_dumb m;
  struct drm_gem_object *obj;

  if(either_copyin(&m, 1, arg, sizeof(m)) < 0 ||
     (obj = vc4_object_by_handle(file, m.handle)) == 0)
    return -1;
  m.offset = obj->mmap_offset;
  return either_copyout(1, arg, &m, sizeof(m));
}

static int
vc4_addfb(struct vc4_firmware_kms *kms, struct drm_file *file, uint64 arg)
{
  struct drm_mode_fb_cmd cmd;
  struct drm_gem_object *obj;
  struct drm_framebuffer *fb;

  if(either_copyin(&cmd, 1, arg, sizeof(cmd)) < 0 ||
     (obj = vc4_object_by_handle(file, cmd.handle)) == 0 ||
     cmd.width != kms->scanout.width || cmd.height != kms->scanout.height ||
     cmd.pitch != kms->scanout.pitch || cmd.bpp != kms->scanout.depth ||
     cmd.depth != 24)
    return -1;
  fb = &file->framebuffers[obj->slot];
  fb->id = VC4_FB_ID_BASE + obj->slot;
  fb->width = cmd.width;
  fb->height = cmd.height;
  fb->pitch = cmd.pitch;
  fb->bpp = cmd.bpp;
  fb->obj = obj;
  cmd.fb_id = fb->id;
  return either_copyout(1, arg, &cmd, sizeof(cmd));
}

static int
vc4_rmfb(struct vc4_firmware_kms *kms, struct drm_file *file, uint64 arg)
{
  uint32 id;
  struct drm_framebuffer *fb;

  if(either_copyin(&id, 1, arg, sizeof(id)) < 0 ||
     (fb = vc4_fb_by_id(file, id)) == 0)
    return -1;
  mutex_lock(&kms->commit_lock);
  acquire(&kms->lock);
  if(kms->current_fb == id){
    release(&kms->lock);
    mutex_unlock(&kms->commit_lock);
    return -1;
  }
  release(&kms->lock);
  memset(fb, 0, sizeof(*fb));
  mutex_unlock(&kms->commit_lock);
  return 0;
}

static int
vc4_page_flip(struct vc4_firmware_kms *kms, struct drm_file *file,
              uint64 arg)
{
  struct drm_mode_crtc_page_flip p;
  struct drm_framebuffer *fb;
  uint64 seq;

  if(either_copyin(&p, 1, arg, sizeof(p)) < 0 ||
     p.crtc_id != VC4_CRTC_ID ||
     (p.flags & ~DRM_MODE_PAGE_FLIP_EVENT) ||
     (fb = vc4_fb_by_id(file, p.fb_id)) == 0)
    return -1;
  mutex_lock(&kms->commit_lock);
  if((kms->native ? vc4_native_page_flip(fb->obj->slot, &seq) :
                    bcm2708_fb_page_flip(fb->obj->slot, &seq)) < 0){
    mutex_unlock(&kms->commit_lock);
    return -1;
  }
  acquire(&kms->lock);
  kms->current_fb = fb->id;
  release(&kms->lock);
  mutex_unlock(&kms->commit_lock);
  p.sequence = seq;
  return either_copyout(1, arg, &p, sizeof(p));
}

static int
vc4_wait_vblank(struct vc4_firmware_kms *kms, uint64 arg)
{
  struct drm_wait_vblank v;

  if((kms->native ? vc4_native_wait_vblank(&v.sequence) :
                    bcm2708_fb_wait_vsync(&v.sequence)) < 0)
    return -1;
  v.timestamp_us = vc4_now_us();
  return either_copyout(1, arg, &v, sizeof(v));
}

static int
vc4_ioctl(struct drm_device *dev, struct file *f, uint64 cmd, uint64 arg)
{
  struct vc4_firmware_kms *kms = dev->dev_private;
  struct drm_file *file = f->private_data;

  if(file == 0)
    return -1;
  switch(cmd){
  case DRM_IOCTL_VERSION:
    return vc4_version(dev, arg);
  case DRM_IOCTL_MODE_GETRESOURCES:
    return vc4_get_resources(kms, arg);
  case DRM_IOCTL_MODE_GETCONNECTOR:
    return vc4_get_connector(kms, arg);
  case DRM_IOCTL_MODE_GETCRTC:
    return vc4_get_crtc(kms, arg);
  case DRM_IOCTL_MODE_CREATE_DUMB:
    return vc4_create_dumb(kms, file, arg);
  case DRM_IOCTL_MODE_MAP_DUMB:
    return vc4_map_dumb(file, arg);
  case DRM_IOCTL_MODE_ADDFB:
    return vc4_addfb(kms, file, arg);
  case DRM_IOCTL_MODE_RMFB:
    return vc4_rmfb(kms, file, arg);
  case DRM_IOCTL_MODE_PAGE_FLIP:
    return vc4_page_flip(kms, file, arg);
  case DRM_IOCTL_WAIT_VBLANK:
    return vc4_wait_vblank(kms, arg);
  default:
    return -1;
  }
}

static int
vc4_mmap(struct drm_device *dev, struct file *f, struct vm_area_struct *vma)
{
  struct drm_file *file = f->private_data;
  uint64 offset = vma->vm_pgoff << PGSHIFT;
  uint64 size = vma->vm_end - vma->vm_start;
  struct drm_gem_object *obj;

  (void)dev;
  if(file == 0 || (obj = vc4_object_by_offset(file, offset)) == 0 ||
     size > obj->size || (vma->vm_flags & VM_EXEC) ||
     !(vma->vm_flags & VM_SHARED))
    return -1;
  return remap_pfn_range(vma, vma->vm_start, obj->paddr >> PGSHIFT,
                         size, PTE_NORMAL_NC);
}

static int
vc4_poll(struct drm_device *dev, struct file *f, int events)
{
  (void)dev;
  (void)f;
  // Page flips are synchronous, so the file is immediately
  // writable and has no queued asynchronous event records yet.
  return events & POLLOUT;
}

static const struct drm_driver vc4_driver = {
  .name = "vc4",
  .desc = "VC4 native HVS KMS with firmware fallback",
  .open = vc4_open,
  .postclose = vc4_postclose,
  .ioctl = vc4_ioctl,
  .mmap = vc4_mmap,
  .poll = vc4_poll,
};

static void
vc4_firmware_kms_init(void)
{
  struct bcm2708_fb_info firmware;

  initlock(&vc4.lock, "vc4-kms");
  mutex_init(&vc4.commit_lock, "vc4-commit");
  if(bcm2708_fb_get_info(&firmware) < 0){
    printf("vc4-drm: firmware double-buffer scanout unavailable\n");
    return;
  }
  if(vc4_native_kms_init(&firmware, &vc4.scanout) == 0)
    vc4.native = 1;
  else
    vc4.scanout = firmware;
  if((vc4.scanout.base & (PGSIZE - 1)) ||
     (vc4.scanout.frame_bytes & (PGSIZE - 1))){
    printf("vc4-drm: scanout buffers are not page aligned\n");
    return;
  }
  vc4.drm.name = "card0";
  vc4.drm.devnode = "dri/card0";
  vc4.drm.major = DRM;
  vc4.drm.minor = 0;
  vc4.drm.driver = &vc4_driver;
  vc4.drm.dev_private = &vc4;
  if(drm_dev_register(&vc4.drm) < 0){
    printf("vc4-drm: cannot register /dev/dri/card0\n");
    return;
  }
  printf("vc4-drm: card0 %s KMS, %dx%d, 2 GEM scanout buffers, "
         "vblank page-flip\n", vc4.native ? "native HVS/PV2" : "firmware",
         vc4.scanout.width, vc4.scanout.height);
}

module_init_ord("vc4-kms", vc4_firmware_kms_init, 20);
