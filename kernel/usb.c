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
  for(id = udrv->id_table; id->vendor || id->product || id->match_flags; id++){
    uint8 flags = id->match_flags;
    // Two-field legacy ID tables mean an exact VID:PID pair (with
    // USB_ANY_ID wildcards), as they did before class matching was added.
    if(flags == 0)
      flags = USB_DEVICE_ID_MATCH_VENDOR | USB_DEVICE_ID_MATCH_PRODUCT;
    if((!(flags & USB_DEVICE_ID_MATCH_VENDOR) || id->vendor == USB_ANY_ID ||
        id->vendor == udev->vendor) &&
       (!(flags & USB_DEVICE_ID_MATCH_PRODUCT) || id->product == USB_ANY_ID ||
        id->product == udev->product) &&
       (!(flags & USB_DEVICE_ID_MATCH_CLASS) ||
        id->class == udev->class) &&
       (!(flags & USB_DEVICE_ID_MATCH_SUBCLASS) ||
        id->subclass == udev->subclass) &&
       (!(flags & USB_DEVICE_ID_MATCH_PROTOCOL) ||
        id->protocol == udev->protocol))
      return 1;
  }
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

struct urb *
usb_alloc_urb(void)
{
  struct urb *urb = kalloc();
  if(urb == 0)
    return 0;
  memset(urb, 0, sizeof(*urb));
  initlock(&urb->lock, "usb-urb");
  urb->state = URB_IDLE;
  return urb;
}

void
usb_free_urb(struct urb *urb)
{
  if(urb == 0)
    return;
  usb_kill_urb(urb);
  kfree(urb);
}

void
usb_fill_int_urb(struct urb *urb, struct usb_device *udev, int endpoint,
                 void *buffer, int length, usb_complete_t complete,
                 void *context, int interval)
{
  if(urb == 0)
    return;
  acquire(&urb->lock);
  if(urb->state == URB_IDLE){
    urb->dev = udev;
    urb->endpoint = endpoint;
    urb->direction_in = 1;
    urb->interval = interval;
    urb->transfer_buffer = buffer;
    urb->transfer_buffer_length = length;
    urb->actual_length = 0;
    urb->status = 0;
    urb->context = context;
    urb->complete = complete;
  }
  release(&urb->lock);
}

int
usb_submit_urb(struct urb *urb)
{
  int r;

  if(urb == 0 || urb->dev == 0 || urb->dev->ops == 0 ||
     urb->dev->ops->submit_urb == 0 || urb->complete == 0 ||
     urb->transfer_buffer == 0 || urb->transfer_buffer_length <= 0)
    return -1;
  acquire(&urb->lock);
  if(urb->state != URB_IDLE || urb->killed){
    release(&urb->lock);
    return -1;
  }
  urb->actual_length = 0;
  urb->status = 0;
  urb->state = URB_SUBMITTED;
  release(&urb->lock);

  r = urb->dev->ops->submit_urb(urb);
  if(r < 0){
    acquire(&urb->lock);
    if(urb->state == URB_SUBMITTED)
      urb->state = URB_IDLE;
    wakeup(urb);
    release(&urb->lock);
  }
  return r;
}

// HCDs call this exactly once for each successfully submitted URB.  Ownership
// returns to the class driver before its completion callback, so the callback
// may queue deferred processing or immediately resubmit the request.
void
usb_hcd_giveback_urb(struct urb *urb, int status, int actual_length)
{
  usb_complete_t complete;

  if(urb == 0)
    return;
  acquire(&urb->lock);
  if(urb->state != URB_SUBMITTED){
    release(&urb->lock);
    return;
  }
  if(urb->killed){
    urb->status = -1;
    urb->actual_length = 0;
    urb->state = URB_KILLED;
    wakeup(urb);
    release(&urb->lock);
    return;
  }
  urb->status = status;
  urb->actual_length = actual_length;
  urb->state = URB_IDLE;
  urb->completing = 1;
  complete = urb->complete;
  release(&urb->lock);

  if(complete)
    complete(urb);

  acquire(&urb->lock);
  urb->completing = 0;
  if(urb->killed && urb->state == URB_IDLE)
    urb->state = URB_KILLED;
  wakeup(urb);
  release(&urb->lock);
}

void
usb_kill_urb(struct urb *urb)
{
  struct usb_device *udev;
  int submitted;

  if(urb == 0)
    return;
  acquire(&urb->lock);
  udev = urb->dev;
  submitted = urb->state == URB_SUBMITTED;
  urb->killed = 1;
  if(urb->state == URB_IDLE)
    urb->state = URB_KILLED;
  release(&urb->lock);

  if(submitted && udev && udev->ops && udev->ops->kill_urb)
    udev->ops->kill_urb(urb);

  acquire(&urb->lock);
  while(urb->completing)
    sleep(urb, &urb->lock);
  urb->state = URB_KILLED;
  release(&urb->lock);
}
