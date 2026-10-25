#ifndef XV6_DEVICE_H
#define XV6_DEVICE_H

#include <kernel/kobject.h>

#define DEVICE_NAME_MAX 32
#define DEVICE_RES_MAX   4
#define DEVICE_MAX      64

enum resource_type {
  IORESOURCE_MEM = 1,
  IORESOURCE_IRQ = 2,
};

struct resource {
  uint64 start;
  uint64 end;
  int type;
  char *name;
};

struct file;
struct inode;

// Open-file operations shared by filesystems and devices. read/write are the
// original stateless character-device hooks (console, tty). Drivers that need per-open state set the optional
// Linux-style hooks: open() may set f->private_data, release() runs when the
// last reference to that open file description is dropped, and fread() is
// used in preference to read() so the driver can see its private state and
// f->flags (O_NONBLOCK).
struct file_operations {
  int (*read)(int user_dst, uint64 dst, int n);
  int (*write)(int user_src, uint64 src, int n);
  int (*open)(struct file *f);
  void (*release)(struct file *f);
  int (*fread)(struct file *f, int user_dst, uint64 dst, int n);
  int (*fwrite)(struct file *f, int user_src, uint64 src, int n);
  // Filesystem file operations use these explicit-offset backends.  The
  // generic fread/fwrite helpers own f->off and call read_at/write_at; pread,
  // pwrite, exec and the module loader use the same callbacks without
  // smuggling data I/O through inode_operations.
  int (*read_at)(struct file *f, int user_dst, uint64 dst, uint64 off,
                 uint n);
  int (*write_at)(struct file *f, int user_src, uint64 src, uint64 off,
                  uint n);
  int (*fsync)(struct file *f, int dataonly);
  int (*poll)(struct file *f, int events);
  int (*ioctl)(struct file *f, uint64 cmd, uint64 arg);
  uint64 (*mmap)(struct file *f, uint64 addr, uint64 len, int prot,
                 int flags, uint64 off);
  int (*munmap)(struct file *f, uint64 addr, uint64 len);
};

struct device;
struct device_driver;

// A device attribute file in sysfs: the kobject attribute (name + mode) plus
// the show/store that back it (Linux device_attribute / DEVICE_ATTR).  The
// generic device sysfs_ops casts its struct attribute* to this type, so the
// embedded `attr` must stay the first member, exactly as in Linux.
struct device_attribute {
  struct attribute attr;
  int (*show)(struct device *dev, struct device_attribute *attr,
              char *buf, int size);
  int (*store)(struct device *dev, struct device_attribute *attr,
               char *buf, int size);
};

#define __dev_str2(x) #x
#define __dev_str(x)  __dev_str2(x)
#define DEVICE_ATTR(_name, _mode, _show, _store)              \
  struct device_attribute dev_attr_##_name =                  \
    { { __dev_str(_name), _mode }, _show, _store }
#define DEVICE_ATTR_RO(_name)                                 \
  DEVICE_ATTR(_name, 0444, _name##_show, 0)
#define DEVICE_ATTR_RW(_name)                                 \
  DEVICE_ATTR(_name, 0644, _name##_show, _name##_store)

struct bus_type {
  char *name;
  struct kobject kobj;         // appears as /sys/bus/<name>
  struct kobject *drivers_dir; // /sys/bus/<name>/drivers, created on register
  int (*match)(struct device *dev, struct device_driver *drv);
  int (*probe)(struct device *dev);
  void (*remove)(struct device *dev);
  // Format this device's MODALIAS into buf (Linux's bus->uevent, narrowed to
  // just the alias string).  Returns the length written, or <= 0 if the device
  // has no alias.  The driver core calls it when a device ends up unbound so
  // request_module() can auto-load the driver that claims this alias.
  int (*modalias)(struct device *dev, char *buf, int size);
};

struct device_driver {
  char *name;
  struct kobject kobj;         // appears as /sys/bus/<bus>/drivers/<name>
  struct bus_type *bus;
  int (*probe)(struct device *dev);
  void (*remove)(struct device *dev);
};

struct device {
  char *name;
  struct kobject kobj;         // appears as /sys/devices/<name>
  struct attribute **driver_attrs;  // attribute groups owned by the driver
  char *class;                 // Linux dev->class name, e.g. "tty"; if set the
                               // core links this device into /sys/class/<class>
  struct kobject *class_link;  // the /sys/class/<class>/<name> symlink, if any
  int id;
  struct device *parent;
  struct bus_type *bus;
  struct resource resource[DEVICE_RES_MAX];
  int nresource;
  struct device_driver *driver;
  void *driver_data;
  // Optional devtmpfs identity.  A registered device with devnode != 0 is
  // represented by /dev/<devnode> for exactly its registered lifetime.
  char *devnode;
  short dev_major;
  short dev_minor;
};

void device_init(void);
int bus_register(struct bus_type *bus);
int bus_unregister(struct bus_type *bus);
int device_register(struct device *dev);
int device_unregister(struct device *dev);
struct device *device_at(int index);
int driver_register(struct device_driver *drv);
int driver_unregister(struct device_driver *drv);
int platform_device_register(struct device *dev);
int platform_driver_register(struct device_driver *drv);
extern struct bus_type platform_bus;
int register_chrdev(int major, char *name, struct file_operations *fops);
int chrdev_open(struct file *f);
int device_format(char *buf, int max);

#endif
