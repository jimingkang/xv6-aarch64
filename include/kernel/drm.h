#ifndef XV6_DRM_H
#define XV6_DRM_H

#include <kernel/device.h>
#include <uapi/drm.h>

#define DRM_MAX_GEM_OBJECTS 2

struct drm_device;

struct drm_gem_object {
  uint32 handle;
  uint32 slot;
  uint64 paddr;
  uint64 size;
  uint64 mmap_offset;
  int refcount;
};

struct drm_framebuffer {
  uint32 id;
  uint32 width;
  uint32 height;
  uint32 pitch;
  uint32 bpp;
  struct drm_gem_object *obj;
};

struct drm_file {
  struct drm_device *dev;
  struct drm_gem_object objects[DRM_MAX_GEM_OBJECTS];
  struct drm_framebuffer framebuffers[DRM_MAX_GEM_OBJECTS];
  uint32 object_count;
};

struct drm_driver {
  char *name;
  char *desc;
  int (*open)(struct drm_device *dev, struct file *f);
  void (*postclose)(struct drm_device *dev, struct file *f);
  int (*ioctl)(struct drm_device *dev, struct file *f, uint64 cmd,
               uint64 arg);
  int (*mmap)(struct drm_device *dev, struct file *f,
              struct vm_area_struct *vma);
  int (*poll)(struct drm_device *dev, struct file *f, int events);
};

struct drm_device {
  char *name;
  char *devnode;
  int major;
  int minor;
  const struct drm_driver *driver;
  void *dev_private;
  struct device dev;
  int registered;
};

int drm_dev_register(struct drm_device *dev);
int drm_dev_unregister(struct drm_device *dev);

#endif
