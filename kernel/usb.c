#include "types.h"
#include "aarch64.h"
#include "defs.h"
#include "device.h"
#include "usb.h"

#define USB_CONTAINER(ptr, type, member) \
  ((type *)((char *)(ptr) - __builtin_offsetof(type, member)))

static int
usb_match(struct device *dev, struct device_driver *driver)
{
  struct usb_device *udev = USB_CONTAINER(dev, struct usb_device, dev);
  struct usb_driver *udrv = USB_CONTAINER(driver, struct usb_driver, driver);
  const struct usb_device_id *id;

  if(udrv->id_table == 0)
    return 0;
  for(id = udrv->id_table; id->vendor || id->product; id++)
    if((id->vendor == USB_ANY_ID || id->vendor == udev->vendor) &&
       (id->product == USB_ANY_ID || id->product == udev->product))
      return 1;
  return 0;
}

static int
usb_probe(struct device *dev)
{
  struct usb_device *udev = USB_CONTAINER(dev, struct usb_device, dev);
  struct usb_driver *udrv;
  if(dev->driver == 0)
    return -1;
  udrv = USB_CONTAINER(dev->driver, struct usb_driver, driver);
  return udrv->probe ? udrv->probe(udev) : 0;
}

static void
usb_remove(struct device *dev)
{
  struct usb_device *udev = USB_CONTAINER(dev, struct usb_device, dev);
  struct usb_driver *udrv;
  if(dev->driver == 0)
    return;
  udrv = USB_CONTAINER(dev->driver, struct usb_driver, driver);
  if(udrv->remove)
    udrv->remove(udev);
}

static struct bus_type usb_bus = {
  .name = "usb",
  .match = usb_match,
  .probe = usb_probe,
  .remove = usb_remove,
};

void
usb_bus_init(void)
{
  bus_register(&usb_bus);
}

int
usb_bus_exit(void)
{
  return bus_unregister(&usb_bus);
}

int
usb_device_register(struct usb_device *udev)
{
  if(udev == 0 || udev->ops == 0 || udev->ops->control == 0)
    return -1;
  udev->dev.bus = &usb_bus;
  return device_register(&udev->dev);
}

int
usb_device_unregister(struct usb_device *udev)
{
  return udev ? device_unregister(&udev->dev) : -1;
}

int
usb_register_driver(struct usb_driver *driver)
{
  if(driver == 0)
    return -1;
  driver->driver.bus = &usb_bus;
  return driver_register(&driver->driver);
}

int
usb_unregister_driver(struct usb_driver *driver)
{
  return driver ? driver_unregister(&driver->driver) : -1;
}
