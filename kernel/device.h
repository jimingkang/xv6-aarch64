#ifndef XV6_DEVICE_H
#define XV6_DEVICE_H

#define DEVICE_NAME_MAX 32
#define DEVICE_RES_MAX   4
#define DEVICE_MAX      32

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
struct vm_area_struct;

// Character-device operations.  read/write are the original stateless hooks
// (console, tty).  Drivers that need per-open state set the optional
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
  // Linux-style ioctl: cmd and arg straight from the system call; arg is
  // usually a user pointer the driver copyout()s to / copyin()s from.
  int (*ioctl)(struct file *f, uint64 cmd, uint64 arg);
  // Linux-style mmap: the core has chosen vma->vm_start/vm_end; the driver
  // checks vma->vm_pgoff and the length and installs its frames with
  // remap_pfn_range() (kernel/mman.h).
  int (*mmap)(struct file *f, struct vm_area_struct *vma);
};

struct device;
struct device_driver;

struct bus_type {
  char *name;
  int (*match)(struct device *dev, struct device_driver *drv);
  int (*probe)(struct device *dev);
  void (*remove)(struct device *dev);
};

struct device_driver {
  char *name;
  struct bus_type *bus;
  int (*probe)(struct device *dev);
  void (*remove)(struct device *dev);
};

struct device {
  char *name;
  int id;
  struct device *parent;
  struct bus_type *bus;
  struct resource resource[DEVICE_RES_MAX];
  int nresource;
  struct device_driver *driver;
  void *driver_data;
};

void device_init(void);
int bus_register(struct bus_type *bus);
int bus_unregister(struct bus_type *bus);
int device_register(struct device *dev);
int device_unregister(struct device *dev);
int driver_register(struct device_driver *drv);
int driver_unregister(struct device_driver *drv);
int platform_device_register(struct device *dev);
int platform_driver_register(struct device_driver *drv);
extern struct bus_type platform_bus;
int register_chrdev(int major, char *name, struct file_operations *fops);
int chrdev_open(struct file *f);
void chrdev_release(struct file *f);
int chrdev_read(struct file *f, int user_dst, uint64 dst, int n);
int chrdev_write(int major, int user_src, uint64 src, int n);
int chrdev_ioctl(struct file *f, uint64 cmd, uint64 arg);
int chrdev_mmap(struct file *f, struct vm_area_struct *vma);
int device_format(char *buf, int max);

#endif
