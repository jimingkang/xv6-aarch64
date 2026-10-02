#include "types.h"
#include "aarch64.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "device.h"
#include "defs.h"

struct chrdev {
  int used;
  char *name;
  struct file_operations *fops;
};

static struct {
  struct spinlock lock;
  struct device *devices[DEVICE_MAX];
  struct device_driver *drivers[DEVICE_MAX];
  struct bus_type *buses[DEVICE_MAX];
  int ndevice;
  int ndriver;
  int nbus;
  struct chrdev chrdevs[NDEV];
} device_core;

static int
platform_match(struct device *dev, struct device_driver *drv)
{
  return strncmp(dev->name, drv->name, strlen(drv->name) + 1) == 0;
}

static int
bus_probe(struct device *dev)
{
  if(dev->driver && dev->driver->probe)
    return dev->driver->probe(dev);
  return 0;
}

static void
bus_remove(struct device *dev)
{
  if(dev->driver && dev->driver->remove)
    dev->driver->remove(dev);
}

struct bus_type platform_bus = {
  .name = "platform",
  .match = platform_match,
  .probe = bus_probe,
  .remove = bus_remove,
};

static int
bind(struct device *dev, struct device_driver *drv)
{
  int r = 0;
  if(dev->driver || dev->bus == 0 || drv->bus != dev->bus ||
     dev->bus->match == 0 || !dev->bus->match(dev, drv))
    return 0;
  dev->driver = drv;
  if(dev->bus->probe && (r = dev->bus->probe(dev)) < 0){
    dev->driver = 0;
    return r;
  }
  return 1;
}

void
device_init(void)
{
  initlock(&device_core.lock, "device-core");
  memset(&device_core.devices, 0, sizeof(device_core.devices));
  memset(&device_core.drivers, 0, sizeof(device_core.drivers));
  memset(&device_core.buses, 0, sizeof(device_core.buses));
  memset(&device_core.chrdevs, 0, sizeof(device_core.chrdevs));
  device_core.ndevice = 0;
  device_core.ndriver = 0;
  device_core.nbus = 0;
  bus_register(&platform_bus);
}

int
bus_register(struct bus_type *bus)
{
  if(bus == 0 || bus->name == 0)
    return -1;
  acquire(&device_core.lock);
  if(device_core.nbus >= DEVICE_MAX){
    release(&device_core.lock);
    return -1;
  }
  device_core.buses[device_core.nbus++] = bus;
  release(&device_core.lock);
  return 0;
}

int
bus_unregister(struct bus_type *bus)
{
  int i;
  if(bus == 0)
    return -1;
  acquire(&device_core.lock);
  for(i = 0; i < device_core.ndevice; i++)
    if(device_core.devices[i]->bus == bus){
      release(&device_core.lock);
      return -1;
    }
  for(i = 0; i < device_core.ndriver; i++)
    if(device_core.drivers[i]->bus == bus){
      release(&device_core.lock);
      return -1;
    }
  for(i = 0; i < device_core.nbus; i++){
    if(device_core.buses[i] != bus)
      continue;
    for(; i + 1 < device_core.nbus; i++)
      device_core.buses[i] = device_core.buses[i + 1];
    device_core.buses[--device_core.nbus] = 0;
    release(&device_core.lock);
    return 0;
  }
  release(&device_core.lock);
  return -1;
}

int
device_register(struct device *dev)
{
  if(dev == 0 || dev->name == 0)
    return -1;
  acquire(&device_core.lock);
  if(device_core.ndevice >= DEVICE_MAX){
    release(&device_core.lock);
    return -1;
  }
  device_core.devices[device_core.ndevice++] = dev;
  release(&device_core.lock);
  for(int i = 0; i < device_core.ndriver; i++)
    if(bind(dev, device_core.drivers[i]) != 0)
      break;
  return 0;
}

int
device_unregister(struct device *dev)
{
  int i;
  if(dev == 0)
    return -1;
  acquire(&device_core.lock);
  for(i = 0; i < device_core.ndevice; i++)
    if(device_core.devices[i] == dev)
      break;
  if(i == device_core.ndevice){
    release(&device_core.lock);
    return -1;
  }
  // Remove the object from discovery before calling the driver.  remove()
  // may unregister children and must not run under the device-core lock.
  for(; i + 1 < device_core.ndevice; i++)
    device_core.devices[i] = device_core.devices[i + 1];
  device_core.devices[--device_core.ndevice] = 0;
  release(&device_core.lock);
  if(dev->driver && dev->bus && dev->bus->remove)
    dev->bus->remove(dev);
  dev->driver = 0;
  dev->driver_data = 0;
  return 0;
}

int
driver_register(struct device_driver *drv)
{
  if(drv == 0 || drv->name == 0)
    return -1;
  acquire(&device_core.lock);
  if(device_core.ndriver >= DEVICE_MAX){
    release(&device_core.lock);
    return -1;
  }
  device_core.drivers[device_core.ndriver++] = drv;
  release(&device_core.lock);
  for(int i = 0; i < device_core.ndevice; i++)
    bind(device_core.devices[i], drv);
  return 0;
}

int
driver_unregister(struct device_driver *drv)
{
  int i;
  if(drv == 0)
    return -1;
  acquire(&device_core.lock);
  for(i = 0; i < device_core.ndriver; i++)
    if(device_core.drivers[i] == drv)
      break;
  if(i == device_core.ndriver){
    release(&device_core.lock);
    return -1;
  }
  for(; i + 1 < device_core.ndriver; i++)
    device_core.drivers[i] = device_core.drivers[i + 1];
  device_core.drivers[--device_core.ndriver] = 0;
  release(&device_core.lock);
  // Unbind all devices owned by this driver.  Keep the devices registered so
  // a replacement driver can bind later.
  for(i = 0; i < device_core.ndevice; i++){
    struct device *dev = device_core.devices[i];
    if(dev->driver != drv)
      continue;
    if(dev->bus && dev->bus->remove)
      dev->bus->remove(dev);
    dev->driver = 0;
    dev->driver_data = 0;
  }
  return 0;
}

int
platform_device_register(struct device *dev)
{
  dev->bus = &platform_bus;
  return device_register(dev);
}

int
platform_driver_register(struct device_driver *drv)
{
  drv->bus = &platform_bus;
  return driver_register(drv);
}

int
register_chrdev(int major, char *name, struct file_operations *fops)
{
  if(major < 0 || major >= NDEV || fops == 0)
    return -1;
  acquire(&device_core.lock);
  if(device_core.chrdevs[major].used){
    release(&device_core.lock);
    return -1;
  }
  device_core.chrdevs[major].used = 1;
  device_core.chrdevs[major].name = name;
  device_core.chrdevs[major].fops = fops;
  release(&device_core.lock);
  return 0;
}

static struct file_operations *
chrdev_fops(int major)
{
  if(major < 0 || major >= NDEV || !device_core.chrdevs[major].used)
    return 0;
  return device_core.chrdevs[major].fops;
}

// Called once per open file description, after sys_open filled in f.
int
chrdev_open(struct file *f)
{
  struct file_operations *fops = chrdev_fops(f->major);
  if(fops == 0)
    return -1;
  return fops->open ? fops->open(f) : 0;
}

// Called by fileclose() when the last fd/dup/fork reference goes away.
void
chrdev_release(struct file *f)
{
  struct file_operations *fops = chrdev_fops(f->major);
  if(fops && fops->release)
    fops->release(f);
}

int
chrdev_read(struct file *f, int user_dst, uint64 dst, int n)
{
  struct file_operations *fops = chrdev_fops(f->major);
  if(fops == 0)
    return -1;
  if(fops->fread)
    return fops->fread(f, user_dst, dst, n);
  if(fops->read == 0)
    return -1;
  return fops->read(user_dst, dst, n);
}

int
chrdev_write(int major, int user_src, uint64 src, int n)
{
  struct file_operations *fops;
  if(major < 0 || major >= NDEV)
    return -1;
  fops = device_core.chrdevs[major].fops;
  if(!device_core.chrdevs[major].used || fops == 0 || fops->write == 0)
    return -1;
  return fops->write(user_src, src, n);
}

static char *
putstr(char *p, char *end, char *s)
{
  while(*s && p < end)
    *p++ = *s++;
  return p;
}

static char *
putnum(char *p, char *end, uint64 value, int hex)
{
  char tmp[24], *digits = "0123456789abcdef";
  int base = hex ? 16 : 10, n = 0;
  if(hex)
    p = putstr(p, end, "0x");
  do {
    tmp[n++] = digits[value % base];
    value /= base;
  } while(value && n < sizeof(tmp));
  while(n && p < end)
    *p++ = tmp[--n];
  return p;
}

int
device_format(char *buf, int max)
{
  char *p = buf, *end = buf + max;
  p = putstr(p, end, "Character devices:\n");
  for(int i = 0; i < NDEV; i++){
    if(!device_core.chrdevs[i].used)
      continue;
    p = putnum(p, end, i, 0);
    p = putstr(p, end, " ");
    p = putstr(p, end, device_core.chrdevs[i].name);
    p = putstr(p, end, "\n");
  }
  p = putstr(p, end, "\nBuses:\n");
  for(int i = 0; i < device_core.nbus; i++){
    p = putstr(p, end, "  ");
    p = putstr(p, end, device_core.buses[i]->name);
    p = putstr(p, end, "\n");
  }
  p = putstr(p, end, "\nDevices:\n");
  for(int i = 0; i < device_core.ndevice; i++){
    struct device *d = device_core.devices[i];
    p = putstr(p, end, d->name);
    p = putstr(p, end, " bus=");
    p = putstr(p, end, d->bus ? d->bus->name : "(none)");
    p = putstr(p, end, " driver=");
    p = putstr(p, end, d->driver ? d->driver->name : "(unbound)");
    if(d->parent){
      p = putstr(p, end, " parent=");
      p = putstr(p, end, d->parent->name);
    }
    p = putstr(p, end, "\n");
    for(int j = 0; j < d->nresource; j++){
      p = putstr(p, end, "  ");
      p = putstr(p, end, d->resource[j].type == IORESOURCE_IRQ ? "irq " : "mem ");
      p = putnum(p, end, d->resource[j].start,
                 d->resource[j].type != IORESOURCE_IRQ);
      if(d->resource[j].type != IORESOURCE_IRQ){
        p = putstr(p, end, "-");
        p = putnum(p, end, d->resource[j].end, 1);
      }
      p = putstr(p, end, " ");
      p = putstr(p, end, d->resource[j].name);
      p = putstr(p, end, "\n");
    }
  }
  return p - buf;
}
