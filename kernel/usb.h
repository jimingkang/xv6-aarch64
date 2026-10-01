#ifndef XV6_USB_H
#define XV6_USB_H

#include "device.h"

#define USB_ANY_ID 0xffff

struct usb_device;

struct usb_host_ops {
  int (*control)(struct usb_device *udev, uint8 type, uint8 request,
                 uint16 value, uint16 index, void *data, uint16 length);
  int (*bulk)(struct usb_device *udev, int endpoint, int in,
              void *data, int length);
  int (*bulk_rx_arm)(struct usb_device *udev, int endpoint,
                     void *data, int length);
  // -2: still pending/rearmed after NAK, -1: hard error, >=0: bytes complete.
  int (*bulk_rx_complete)(struct usb_device *udev, int endpoint);
};

struct usb_device {
  struct device dev;
  const struct usb_host_ops *ops;
  void *host_private;
  uint16 vendor;
  uint16 product;
  uint8 class;
  uint8 subclass;
  uint8 protocol;
  uint8 address;
  uint8 ep0_max_packet;
  uint8 bulk_in_ep;
  uint8 bulk_in_ep2;
  uint8 bulk_out_ep;
  uint16 bulk_in_max_packet;
  uint16 bulk_out_max_packet;
  uint8 bulk_in_toggle;
  uint8 bulk_in_toggle2;
  uint8 bulk_out_toggle;
};

struct usb_device_id {
  uint16 vendor;
  uint16 product;
};

struct usb_driver {
  struct device_driver driver;
  const struct usb_device_id *id_table;
  int (*probe)(struct usb_device *udev);
  void (*remove)(struct usb_device *udev);
};

void usb_bus_init(void);
int usb_bus_exit(void);
int usb_device_register(struct usb_device *udev);
int usb_device_unregister(struct usb_device *udev);
int usb_register_driver(struct usb_driver *driver);
int usb_unregister_driver(struct usb_driver *driver);

#endif
