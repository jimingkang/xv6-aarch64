#ifndef XV6_USB_H
#define XV6_USB_H

#include "device.h"
#include "spinlock.h"

#define USB_ANY_ID 0xffff

struct usb_device;
struct urb;

typedef void (*usb_complete_t)(struct urb *urb);

enum urb_state {
  URB_IDLE = 0,
  URB_SUBMITTED,
  URB_COMPLETING,
  URB_KILLED,
};

// A deliberately small Linux-like USB Request Block.  USB class drivers own
// the request and its buffer; the host-controller driver owns them only from
// usb_submit_urb() until usb_hcd_giveback_urb() or usb_kill_urb().
struct urb {
  struct spinlock lock;
  struct usb_device *dev;
  int endpoint;
  int direction_in;
  int interval;
  void *transfer_buffer;
  int transfer_buffer_length;
  int actual_length;
  int status;
  void *context;
  usb_complete_t complete;
  int state;
  int killed;
  int completing;
};

struct usb_host_ops {
  int (*control)(struct usb_device *udev, uint8 type, uint8 request,
                 uint16 value, uint16 index, void *data, uint16 length);
  int (*bulk)(struct usb_device *udev, int endpoint, int in,
              void *data, int length);
  int (*bulk_rx_arm)(struct usb_device *udev, int endpoint,
                     void *data, int length);
  // -2: still pending/rearmed after NAK, -1: hard error, >=0: bytes complete.
  int (*bulk_rx_complete)(struct usb_device *udev, int endpoint, void **data);
  int (*submit_urb)(struct urb *urb);
  void (*kill_urb)(struct urb *urb);
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
  uint8 interface_number;
  uint8 interrupt_in_ep;
  uint16 interrupt_in_max_packet;
  uint8 interrupt_in_interval;
  uint8 interrupt_in_toggle;
};

#define USB_DEVICE_ID_MATCH_VENDOR   (1U << 0)
#define USB_DEVICE_ID_MATCH_PRODUCT  (1U << 1)
#define USB_DEVICE_ID_MATCH_CLASS    (1U << 2)
#define USB_DEVICE_ID_MATCH_SUBCLASS (1U << 3)
#define USB_DEVICE_ID_MATCH_PROTOCOL (1U << 4)

struct usb_device_id {
  uint16 vendor;
  uint16 product;
  uint8 class;
  uint8 subclass;
  uint8 protocol;
  uint8 match_flags;
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
struct urb *usb_alloc_urb(void);
void usb_free_urb(struct urb *urb);
void usb_fill_int_urb(struct urb *urb, struct usb_device *udev, int endpoint,
                      void *buffer, int length, usb_complete_t complete,
                      void *context, int interval);
int usb_submit_urb(struct urb *urb);
void usb_kill_urb(struct urb *urb);
void usb_hcd_giveback_urb(struct urb *urb, int status, int actual_length);
void usbkbd_driver_init(void);
void usbkbd_driver_exit(void);

#endif
