#ifndef XV6_DEVICE_H
#define XV6_DEVICE_H

#define DEVICE_NAME_MAX 32
#define DEVICE_RES_MAX   4
#define DEVICE_MAX      16

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

struct file_operations {
  int (*read)(int user_dst, uint64 dst, int n);
  int (*write)(int user_src, uint64 src, int n);
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
int device_register(struct device *dev);
int driver_register(struct device_driver *drv);
int platform_device_register(struct device *dev);
int platform_driver_register(struct device_driver *drv);
extern struct bus_type platform_bus;
int register_chrdev(int major, char *name, struct file_operations *fops);
int chrdev_read(int major, int user_dst, uint64 dst, int n);
int chrdev_write(int major, int user_src, uint64 src, int n);
int device_format(char *buf, int max);

#endif
