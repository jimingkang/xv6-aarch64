#include "types.h"
#include "aarch64.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "device.h"
#include "module.h"
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

// ---------------------------------------------------------------------------
// sysfs views of the device model.
//
// Linux's driver core (drivers/base/core.c) does not print a text summary of
// devices; it hangs a kobject off each struct device / device_driver /
// bus_type and lets the generic sysfs walker render them.  We do the same:
// each registration creates the matching kobject under /sys/devices and
// /sys/bus, and every attribute file is a (kobject, attribute) pair whose
// show/store the ktype dispatches below.  Nothing in this block hard-codes a
// driver -- adding one that registers a device makes its files appear.

static struct kobject *devices_kobj;   // /sys/devices
static struct kobject *bus_dir_kobj;   // /sys/bus
static struct kobject *class_dir_kobj; // /sys/class

// Format one attribute value: the string, then a newline, as sysfs wants.
static int
sys_println(char *buf, int size, char *s)
{
  int n = strlen(s);
  if(n > size - 2)
    n = size - 2;
  if(n < 0)
    n = 0;
  memmove(buf, s, n);
  buf[n++] = '\n';
  buf[n] = 0;
  return n;
}

// Linux sysfs has no '/' inside one name (it is a directory component), so a
// device named "input/event0" is filed as "input!event0".  Reproduce that.
static void
safe_kobj_name(char *out, char *in)
{
  int i;
  for(i = 0; in[i] && i < KOBJ_NAME_MAX - 1; i++)
    out[i] = in[i] == '/' ? '!' : in[i];
  out[i] = 0;
}

// -- device attributes (the ones every device gets) -------------------------

static int
dev_name_show(struct device *dev, struct device_attribute *attr, char *buf,
              int size)
{
  return sys_println(buf, size, dev->name);
}

static int
dev_subsystem_show(struct device *dev, struct device_attribute *attr,
                   char *buf, int size)
{
  return sys_println(buf, size, dev->bus ? dev->bus->name : "");
}

// Linux files the bound driver as a symlink; without symlinks this port
// exposes the driver's name here, which a reader can then look up under
// /sys/bus/<bus>/drivers.  Empty when the device is unbound.
static int
dev_driver_show(struct device *dev, struct device_attribute *attr, char *buf,
               int size)
{
  return sys_println(buf, size, dev->driver ? dev->driver->name : "");
}

static int
dev_modalias_show(struct device *dev, struct device_attribute *attr, char *buf,
                  int size)
{
  char alias[MODALIAS_MAX];
  if(dev->bus && dev->bus->modalias &&
     dev->bus->modalias(dev, alias, sizeof(alias)) > 0)
    return sys_println(buf, size, alias);
  buf[0] = 0;
  return 0;
}

// Append "key=value\n" at offset pos of buf and return the new length.  Used
// by the uevent file, which is a small KEY=VALUE environment dump, as in
// Linux's kobject uevent sysfs attribute.
static int
sys_kvline(char *buf, int size, int pos, char *k, char *v)
{
  int i = 0;
  for(; k[i] && pos < size - 1; i++)
    buf[pos++] = k[i];
  if(pos < size - 1)
    buf[pos++] = '=';
  for(i = 0; v[i] && pos < size - 1; i++)
    buf[pos++] = v[i];
  if(pos < size - 1)
    buf[pos++] = '\n';
  return pos;
}

static int
dev_uevent_show(struct device *dev, struct device_attribute *attr, char *buf,
                int size)
{
  char alias[MODALIAS_MAX];
  int have = dev->bus && dev->bus->modalias &&
             dev->bus->modalias(dev, alias, sizeof(alias)) > 0;
  int n = 0;
  n = sys_kvline(buf, size, n, "DRIVER", dev->driver ? dev->driver->name : "");
  n = sys_kvline(buf, size, n, "MODALIAS", have ? alias : "");
  return n;
}

// Writing an action ("add", "remove", "change", "online", "offline") to a
// device's uevent file is how user space re-runs the hotplug path by hand.
// A device that has no driver but does have a MODALIAS asks kmod to load the
// matching module -- the same request_module() the core fires on auto-bind.
static int
dev_uevent_store(struct device *dev, struct device_attribute *attr, char *buf,
                 int size)
{
  char alias[MODALIAS_MAX];
  char *action = buf;
  int have = dev->bus && dev->bus->modalias &&
             dev->bus->modalias(dev, alias, sizeof(alias)) > 0;
  while(*action == ' ' || *action == '\t')
    action++;
  // echo appends a newline; terminate the action token so the printf and the
  // strncmp() matches below see a clean "add"/"change"/"remove".
  char *p = action;
  while(*p && *p != '\n' && *p != '\r' && *p != ' ' && *p != '\t')
    p++;
  *p = 0;
  // xv6's echo writes the argument and the newline as two separate write()
  // calls, so store() runs twice.  An empty action (the trailing newline) is
  // a no-op, as it is in Linux's uevent_store -- only log/act on a real token.
  if(*action == 0)
    return size;
  printf("uevent: %s %s%s%s\n", action, dev->name,
         have ? " MODALIAS=" : "", have ? alias : "");
  if((strncmp(action, "add", 4) == 0 || strncmp(action, "change", 7) == 0) &&
     dev->driver == 0 && have)
    request_module(alias);
  return size;
}

static struct device_attribute dev_attr_name      = { { "name", 0444 },      dev_name_show, 0 };
static struct device_attribute dev_attr_subsystem = { { "subsystem", 0444 }, dev_subsystem_show, 0 };
static struct device_attribute dev_attr_driver    = { { "driver", 0444 },    dev_driver_show, 0 };
static struct device_attribute dev_attr_modalias  = { { "modalias", 0444 },  dev_modalias_show, 0 };
static struct device_attribute dev_attr_uevent    = { { "uevent", 0644 },    dev_uevent_show, dev_uevent_store };

static struct attribute *device_default_attrs[] = {
  &dev_attr_name.attr, &dev_attr_subsystem.attr,
  &dev_attr_driver.attr, &dev_attr_modalias.attr, &dev_attr_uevent.attr, 0
};

// A device's own attributes (declared with DEVICE_ATTR and listed in
// dev->driver_attrs) are device_attribute objects too, so one sysfs_ops
// dispatch serves both the defaults and the per-driver files.
static int
device_sysfs_show(struct kobject *kobj, struct attribute *attr, char *buf,
                  int size)
{
  struct device *dev = kobj->priv;
  struct device_attribute *da = (struct device_attribute *) attr;
  if(dev == 0 || da->show == 0)
    return -1;
  return da->show(dev, da, buf, size);
}

static int
device_sysfs_store(struct kobject *kobj, struct attribute *attr, char *buf,
                   int size)
{
  struct device *dev = kobj->priv;
  struct device_attribute *da = (struct device_attribute *) attr;
  if(dev == 0 || da->store == 0)
    return -1;
  return da->store(dev, da, buf, size);
}

static struct sysfs_ops device_sysfs_ops = {
  .show = device_sysfs_show,
  .store = device_sysfs_store,
};

static void
device_ktype_release(struct kobject *kobj)
{
  // Built-in devices live in .data/.bss and are never freed; the last
  // reference only means the object left the tree, which kobject_put did.
  (void) kobj;
}

static struct kobj_type device_ktype = {
  .release = device_ktype_release,
  .sysfs_ops = &device_sysfs_ops,
  .default_attrs = device_default_attrs,
};

// -- driver and bus objects -------------------------------------------------

static struct attribute driver_name_attr = { "name", 0444 };
static struct attribute *driver_default_attrs[] = { &driver_name_attr, 0 };

static int
driver_sysfs_show(struct kobject *kobj, struct attribute *attr, char *buf,
                  int size)
{
  struct device_driver *drv = kobj->priv;
  if(drv == 0)
    return -1;
  return sys_println(buf, size, drv->name);
}

static struct sysfs_ops driver_sysfs_ops = {
  .show = driver_sysfs_show,
  .store = 0,
};

static struct kobj_type driver_ktype = {
  .release = 0,
  .sysfs_ops = &driver_sysfs_ops,
  .default_attrs = driver_default_attrs,
};

static struct attribute bus_name_attr = { "name", 0444 };
static struct attribute bus_drivers_probe_attr = { "drivers_probe", 0200 };
static struct attribute *bus_default_attrs[] = {
  &bus_name_attr, &bus_drivers_probe_attr, 0
};

static int
bus_sysfs_show(struct kobject *kobj, struct attribute *attr, char *buf,
               int size)
{
  struct bus_type *bus = kobj->priv;
  if(bus == 0)
    return -1;
  if(strncmp(attr->name, "name", 5) == 0)
    return sys_println(buf, size, bus->name);
  buf[0] = 0;                    // drivers_probe is write-only
  return 0;
}

// echo <device-name> > /sys/bus/<bus>/drivers_probe: the manual cold-plug
// re-bind.  Find the named device on this bus and, if nothing has claimed it
// yet, offer it to every driver in turn (Linux __driver_attach over the bus).
static int
bus_sysfs_store(struct kobject *kobj, struct attribute *attr, char *buf,
                int size)
{
  struct bus_type *bus = kobj->priv;
  char name[DEVICE_NAME_MAX];
  int n = 0;

  if(bus == 0 || strncmp(attr->name, "drivers_probe", 14) != 0)
    return -1;
  while(buf[n] && buf[n] != '\n' && buf[n] != ' ' && n < DEVICE_NAME_MAX - 1){
    name[n] = buf[n];
    n++;
  }
  name[n] = 0;
  // xv6's echo writes the name and the newline as two write() calls; an empty
  // name (the trailing newline) is a no-op, accepted without an error.
  if(n == 0)
    return size;

  // Read the device list unlocked, as the register/unregister bind loops do:
  // a driver's probe may sleep, so bind() must not run under device_core.lock.
  for(int i = 0; i < device_core.ndevice; i++){
    struct device *dev = device_core.devices[i];
    if(dev->bus != bus || strncmp(dev->name, name, strlen(name) + 1) != 0)
      continue;
    if(dev->driver == 0)
      for(int j = 0; j < device_core.ndriver; j++)
        if(bind(dev, device_core.drivers[j]))
          break;
    return size;
  }
  return -1;
}

static struct sysfs_ops bus_sysfs_ops = {
  .show = bus_sysfs_show,
  .store = bus_sysfs_store,
};

static struct kobj_type bus_ktype = {
  .release = 0,
  .sysfs_ops = &bus_sysfs_ops,
  .default_attrs = bus_default_attrs,
};

// ---------------------------------------------------------------------------

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
  // Build the two top-level sysfs directories before any bus registers; the
  // root itself was created by kobject_subsys_init() in main().
  devices_kobj = kobject_create_and_add("devices", kobject_sys_root());
  bus_dir_kobj = kobject_create_and_add("bus", kobject_sys_root());
  class_dir_kobj = kobject_create_and_add("class", kobject_sys_root());
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
  // Give the bus its /sys/bus/<name> node and a drivers/ directory for the
  // drivers that register on it.
  kobject_init(&bus->kobj, &bus_ktype);
  bus->kobj.priv = bus;
  kobject_add(&bus->kobj, bus_dir_kobj, bus->name);
  bus->drivers_dir = kobject_create_and_add("drivers", &bus->kobj);
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
    kobject_del(&bus->kobj);
    return 0;
  }
  release(&device_core.lock);
  return -1;
}

// Find or create /sys/class/<name>, the functional grouping Linux builds with
// class_register().  Unlike /sys/bus (which mirrors a driver core bus), a class
// groups devices that do the same job across different buses, so devices link
// in by name rather than being parented.  Reuses the shared synth pool.
static struct kobject*
class_dir_for(char *name)
{
  struct kobject *cd;
  if(class_dir_kobj == 0 || name == 0 || name[0] == 0)
    return 0;
  if((cd = kobject_child_by_name(class_dir_kobj, name)) != 0)
    return cd;
  return kobject_create_and_add(name, class_dir_kobj);
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
  // File the device under /sys/devices/<name>, with the driver's own
  // attribute groups layered on top of the core device attributes.
  {
    char nm[KOBJ_NAME_MAX];
    kobject_init(&dev->kobj, &device_ktype);
    dev->kobj.priv = dev;
    dev->kobj.attrs = dev->driver_attrs;
    safe_kobj_name(nm, dev->name);
    kobject_add(&dev->kobj, devices_kobj, nm);
    // device_add_class_symlinks(): if the device declares a functional class,
    // hang a symlink /sys/class/<class>/<name> pointing back at its real
    // object under /sys/devices.  Traversing or reading it dereferences.
    if(dev->class){
      struct kobject *cd = class_dir_for(dev->class);
      if(cd)
        dev->class_link = kobject_link_add(nm, cd, &dev->kobj);
    }
  }
  // Try every driver already on this device's bus.  bind() returns non-zero
  // when a driver claims the device, whether or not its probe then succeeds, so
  // `matched` records "some loaded driver is responsible for this device."
  int matched = 0;
  for(int i = 0; i < device_core.ndriver; i++)
    if(bind(dev, device_core.drivers[i]) != 0){
      matched = 1;
      break;
    }

  // No loaded driver claimed this device.  Ask the module loader to bring in
  // the .ko whose modules.alias answers this device's MODALIAS -- the cold/hot
  // plug auto-load path Linux drives with a uevent + modprobe.  The request
  // runs off the device-core lock (released above) and, because reading the .ko
  // must sleep, off this context too: request_module() defers the load to the
  // kmod workqueue, whose driver_register() pass then binds this very device.
  if(!matched && dev->bus && dev->bus->modalias){
    char alias[MODALIAS_MAX];
    if(dev->bus->modalias(dev, alias, sizeof(alias)) > 0)
      request_module(alias);
  }
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
  if(dev->class_link){
    kobject_del(dev->class_link);
    dev->class_link = 0;
  }
  kobject_del(&dev->kobj);
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
  // /sys/bus/<bus>/drivers/<name>.  Fall back to /sys/bus if the driver has
  // no bus (or its bus predates drivers_dir) so it still shows up.
  {
    char nm[KOBJ_NAME_MAX];
    kobject_init(&drv->kobj, &driver_ktype);
    drv->kobj.priv = drv;
    safe_kobj_name(nm, drv->name);
    kobject_add(&drv->kobj,
                drv->bus && drv->bus->drivers_dir ? drv->bus->drivers_dir
                                                  : bus_dir_kobj,
                nm);
  }
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
  kobject_del(&drv->kobj);
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
  devtmpfs_notify(major);     // late drivers get their /dev node too
  return 0;
}

// Name under /dev of a registered character device ("console",
// "input/event0"), or 0.  devtmpfs walks majors 0..NDEV-1 with this.
char*
chrdev_name(int major)
{
  if(major < 0 || major >= NDEV || !device_core.chrdevs[major].used)
    return 0;
  return device_core.chrdevs[major].name;
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
chrdev_write(struct file *f, int user_src, uint64 src, int n)
{
  struct file_operations *fops;
  int major = f->major;
  if(major < 0 || major >= NDEV)
    return -1;
  fops = device_core.chrdevs[major].fops;
  if(!device_core.chrdevs[major].used || fops == 0)
    return -1;
  if(fops->fwrite)
    return fops->fwrite(f, user_src, src, n);
  if(fops->write == 0)
    return -1;
  return fops->write(user_src, src, n);
}

uint64
chrdev_mmap(struct file *f, uint64 addr, uint64 len, int prot, int flags,
            uint64 off)
{
  struct file_operations *fops = chrdev_fops(f->major);
  if(fops == 0 || fops->mmap == 0)
    return (uint64)-1;
  return fops->mmap(f, addr, len, prot, flags, off);
}

int
chrdev_munmap(struct file *f, uint64 addr, uint64 len)
{
  struct file_operations *fops = chrdev_fops(f->major);
  if(fops == 0 || fops->munmap == 0)
    return -1;
  return fops->munmap(f, addr, len);
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
