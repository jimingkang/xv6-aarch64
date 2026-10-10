// USB HID boot-protocol mouse driver.
//
// The class driver consumes the standard boot-mouse report (buttons, X, Y,
// optional wheel) from an interrupt-IN URB and publishes Linux-compatible
// EV_KEY/EV_REL frames through the common input/evdev layer.

#include "types.h"
#include "param.h"
#include "aarch64.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "proc.h"
#include "file.h"
#include "defs.h"
#include "device.h"
#include "input.h"
#include "usb.h"
#include "workqueue.h"

#define HID_REQ_SET_IDLE      0x0a
#define HID_REQ_SET_PROTOCOL  0x0b
#define HID_BOOT_PROTOCOL     0
#define HID_MOUSE_REPORT_SIZE 4

struct usbmouse_state {
  struct spinlock lock;
  struct usb_device *udev;
  struct input_dev *input;
  struct urb *irq_urb;
  struct work_struct rx_work;
  uchar report[64] __attribute__((aligned(64)));
  uchar buttons;
  int active;
  int disconnected;
  uint report_count;
  uint error_count;
};

static struct workqueue usbmouse_wq;

static int
signed_byte(uchar value)
{
  return value & 0x80 ? (int)value - 256 : value;
}

static void
usbmouse_rx_work(struct work_struct *work)
{
  struct usbmouse_state *mouse =
    (struct usbmouse_state *)((char *)work -
      __builtin_offsetof(struct usbmouse_state, rx_work));
  static const ushort button_codes[5] = {
    BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA
  };
  int n, status, rearm;

  acquire(&mouse->lock);
  if(!mouse->active || mouse->udev == 0 || mouse->irq_urb == 0){
    release(&mouse->lock);
    return;
  }
  release(&mouse->lock);

  status = mouse->irq_urb->status;
  n = mouse->irq_urb->actual_length;
  if(status < 0){
    if(mouse->error_count++ < 4)
      printf("usbmouse: interrupt transfer error; rearming\n");
  } else if(n >= 3){
    uchar buttons = mouse->report[0];

#if USB_XFER_TRACE
    printf("usbmouse-xfer: report=%d raw=%x %x %x %x buttons=%x "
           "dx=%d dy=%d wheel=%d\n", n, mouse->report[0],
           mouse->report[1], mouse->report[2],
           n >= 4 ? mouse->report[3] : 0, buttons,
           signed_byte(mouse->report[1]), signed_byte(mouse->report[2]),
           n >= 4 ? signed_byte(mouse->report[3]) : 0);
#endif

    if(mouse->report_count++ == 0)
      printf("usbmouse: first report buttons=%x dx=%d dy=%d wheel=%d\n",
             buttons, signed_byte(mouse->report[1]),
             signed_byte(mouse->report[2]),
             n >= 4 ? signed_byte(mouse->report[3]) : 0);
    for(int i = 0; i < 5; i++){
      int old_down = (mouse->buttons & (1U << i)) != 0;
      int new_down = (buttons & (1U << i)) != 0;
      if(old_down != new_down)
        input_report_key(mouse->input, button_codes[i], new_down);
    }
    input_report_rel(mouse->input, REL_X, signed_byte(mouse->report[1]));
    input_report_rel(mouse->input, REL_Y, signed_byte(mouse->report[2]));
    if(n >= 4)
      input_report_rel(mouse->input, REL_WHEEL,
                       signed_byte(mouse->report[3]));
    input_sync(mouse->input);
    mouse->buttons = buttons;
  }

  acquire(&mouse->lock);
  rearm = mouse->active && !mouse->disconnected;
  release(&mouse->lock);
  if(rearm && usb_submit_urb(mouse->irq_urb) < 0){
    acquire(&mouse->lock);
    if(mouse->active && mouse->error_count++ < 4)
      printf("usbmouse: cannot resubmit interrupt URB\n");
    release(&mouse->lock);
  }
}

static void
usbmouse_irq_complete(struct urb *urb)
{
  struct usbmouse_state *mouse = urb ? urb->context : 0;

  if(mouse == 0)
    return;
  acquire(&mouse->lock);
  if(mouse->active && !mouse->disconnected)
    queue_work(&usbmouse_wq, &mouse->rx_work);
  release(&mouse->lock);
}

static int
usbmouse_probe(struct usb_device *udev)
{
  struct usbmouse_state *mouse;

  if(udev == 0 || udev->ops == 0 || udev->ops->submit_urb == 0 ||
     udev->ops->kill_urb == 0 || udev->class != 3 ||
     udev->subclass != 1 || udev->protocol != 2 ||
     udev->interrupt_in_ep == 0)
    return -1;
  mouse = kalloc();
  if(mouse == 0)
    return -1;
  memset(mouse, 0, sizeof(*mouse));
  initlock(&mouse->lock, "usbmouse");
  mouse->udev = udev;
  mouse->input = input_allocate_device();
  mouse->irq_urb = usb_alloc_urb();
  if(mouse->input == 0 || mouse->irq_urb == 0)
    goto fail;
  init_work(&mouse->rx_work, usbmouse_rx_work);

#if USB_ENUM_TRACE
  printf("usbmouse-probe: addr=%d vid=%x pid=%x class=%d subclass=%d "
         "protocol=%d interface=%d ep=%d mps=%d interval=%d\n",
         udev->address, udev->vendor, udev->product, udev->class,
         udev->subclass, udev->protocol, udev->interface_number,
         udev->interrupt_in_ep, udev->interrupt_in_max_packet,
         udev->interrupt_in_interval);
#endif

  // Boot protocol gives a descriptor-independent buttons/X/Y report.
  if(udev->ops->control(udev, 0x21, HID_REQ_SET_PROTOCOL,
                        HID_BOOT_PROTOCOL, udev->interface_number,
                        0, 0) < 0)
    goto fail;
  udev->ops->control(udev, 0x21, HID_REQ_SET_IDLE, 0,
                     udev->interface_number, 0, 0);

  mouse->input->name = "USB HID Boot Mouse";
  mouse->input->phys = "usb/dwc2/input-mouse";
  mouse->input->bustype = 3; // BUS_USB
  mouse->input->vendor = udev->vendor;
  mouse->input->product = udev->product;
  mouse->input->private = mouse;
  input_set_capability(mouse->input, EV_KEY, BTN_LEFT);
  input_set_capability(mouse->input, EV_KEY, BTN_RIGHT);
  input_set_capability(mouse->input, EV_KEY, BTN_MIDDLE);
  input_set_capability(mouse->input, EV_KEY, BTN_SIDE);
  input_set_capability(mouse->input, EV_KEY, BTN_EXTRA);
  input_set_capability(mouse->input, EV_REL, REL_X);
  input_set_capability(mouse->input, EV_REL, REL_Y);
  input_set_capability(mouse->input, EV_REL, REL_WHEEL);
  if(input_register_device(mouse->input) < 0)
    goto fail;

  usb_fill_int_urb(mouse->irq_urb, udev, udev->interrupt_in_ep,
                   mouse->report, HID_MOUSE_REPORT_SIZE,
                   usbmouse_irq_complete, mouse,
                   udev->interrupt_in_interval);
  mouse->active = 1;
  udev->dev.driver_data = mouse;
  if(usb_submit_urb(mouse->irq_urb) < 0)
    goto fail_registered;
  printf("usbmouse: IRQ-driven boot mouse ready event%d ep=%d mps=%d interval=%d\n",
         mouse->input->event_number, udev->interrupt_in_ep,
         udev->interrupt_in_max_packet, udev->interrupt_in_interval);
  return 0;

fail_registered:
  udev->dev.driver_data = 0;
  mouse->active = 0;
  input_unregister_device(mouse->input);
fail:
  if(mouse->irq_urb)
    usb_free_urb(mouse->irq_urb);
  if(mouse->input)
    input_free_device(mouse->input);
  kfree(mouse);
  return -1;
}

static void
usbmouse_remove(struct usb_device *udev)
{
  struct usbmouse_state *mouse = udev ? udev->dev.driver_data : 0;

  if(mouse == 0)
    return;
  acquire(&mouse->lock);
  mouse->active = 0;
  mouse->disconnected = 1;
  release(&mouse->lock);
  usb_kill_urb(mouse->irq_urb);
  cancel_work_sync(&mouse->rx_work);
  input_unregister_device(mouse->input);
  udev->dev.driver_data = 0;
  usb_free_urb(mouse->irq_urb);
  input_free_device(mouse->input);
  kfree(mouse);
}

static const struct usb_device_id usbmouse_ids[] = {
  {
    .class = 3,
    .subclass = 1,
    .protocol = 2,
    .match_flags = USB_DEVICE_ID_MATCH_CLASS |
                   USB_DEVICE_ID_MATCH_SUBCLASS |
                   USB_DEVICE_ID_MATCH_PROTOCOL,
  },
  { 0 },
};

static struct usb_driver usbmouse_driver = {
  .driver = { .name = "usbhid-mouse" },
  .id_table = usbmouse_ids,
  .probe = usbmouse_probe,
  .remove = usbmouse_remove,
};

void
usbmouse_driver_init(void)
{
  init_workqueue(&usbmouse_wq, "usbmouse_wq", 1);
  usb_register_driver(&usbmouse_driver);
}

void
usbmouse_driver_exit(void)
{
  usb_unregister_driver(&usbmouse_driver);
}
