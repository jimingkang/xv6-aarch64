// Minimal DRM device core.  It provides the common /dev/dri/card0 character
// file and delegates policy/hardware operations to one DRM driver.

#include <kernel/types.h>
#include <kernel/param.h>
#include "aarch64.h"
#include <kernel/fs.h>
#include <kernel/file.h>
#include <kernel/defs.h>
#include <kernel/drm.h>

static struct drm_device *card0;

static int
drm_open_file(struct file *f)
{
  if(card0 == 0 || card0->driver == 0)
    return -1;
  f->private_data = 0;
  return card0->driver->open ? card0->driver->open(card0, f) : 0;
}

static void
drm_release_file(struct file *f)
{
  if(card0 && card0->driver && card0->driver->postclose)
    card0->driver->postclose(card0, f);
  f->private_data = 0;
}

static int
drm_ioctl_file(struct file *f, uint64 cmd, uint64 arg)
{
  if(card0 == 0 || card0->driver == 0 || card0->driver->ioctl == 0)
    return -1;
  return card0->driver->ioctl(card0, f, cmd, arg);
}

static int
drm_mmap_file(struct file *f, struct vm_area_struct *vma)
{
  if(card0 == 0 || card0->driver == 0 || card0->driver->mmap == 0)
    return -1;
  return card0->driver->mmap(card0, f, vma);
}

static int
drm_poll_file(struct file *f, int events)
{
  if(card0 == 0 || card0->driver == 0 || card0->driver->poll == 0)
    return 0;
  return card0->driver->poll(card0, f, events);
}

int
drm_dev_register(struct drm_device *dev)
{
  static struct file_operations drm_fops = {
    .open = drm_open_file,
    .release = drm_release_file,
    .ioctl = drm_ioctl_file,
    .mmap = drm_mmap_file,
    .poll = drm_poll_file,
  };

  if(dev == 0 || card0 || dev->driver == 0 || dev->name == 0 ||
     dev->devnode == 0 || dev->major < 0 || dev->minor < 0)
    return -1;
  if(register_chrdev(dev->major, dev->devnode, &drm_fops) < 0)
    return -1;
  dev->dev.name = dev->name;
  dev->dev.class = "drm";
  dev->dev.devnode = dev->devnode;
  dev->dev.dev_major = dev->major;
  dev->dev.dev_minor = dev->minor;
  dev->dev.driver_data = dev;
  if(device_register(&dev->dev) < 0)
    return -1;
  card0 = dev;
  dev->registered = 1;
  return 0;
}

int
drm_dev_unregister(struct drm_device *dev)
{
  if(dev == 0 || dev != card0 || !dev->registered)
    return -1;
  if(device_unregister(&dev->dev) < 0)
    return -1;
  card0 = 0;
  dev->registered = 0;
  return 0;
}
