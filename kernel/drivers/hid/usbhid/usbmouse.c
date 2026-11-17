/* USB HID boot-protocol mouse driver (Linux drivers/hid/usbhid/usbmouse.c).
 *
 * Binds to interface class 3 (HID), subclass 1 (boot), protocol 2 (mouse).
 * Same transport as usbkbd: an interrupt-IN URB on the EHCI periodic
 * schedule (split through the board's USB2514 hub), completed in IRQ 55,
 * processed in a work item, then resubmitted:
 *
 *   ehci_irq -> usb_hcd_giveback_urb -> usbmouse_irq   (interrupt context)
 *     -> queue_work(usbmouse_wq)
 *   usbmouse_rx_work -> input_report_key(BTN_*) / input_report_rel(REL_*)
 *     -> input_sync -> evdev (/dev/input/eventN) and win32k (cursor)
 *     -> usb_submit_urb() again
 *
 * Boot report: byte 0 buttons (b0 left, b1 right, b2 middle, b3/b4 side/
 * extra on 5-button mice), byte 1 X and byte 2 Y as signed 8-bit deltas
 * (+X right, +Y down).  Most mice append a signed wheel byte; like Linux,
 * it is used when the report is at least 4 bytes long.
 */
#include "kernel/types.h"
#include "kernel/aarch64.h"
#include "kernel/param.h"
#include "kernel/spinlock.h"
#include "kernel/defs.h"
#include "kernel/device.h"
#include "kernel/input.h"
#include "kernel/module.h"
#include "kernel/usb.h"
#include "kernel/workqueue.h"

/* USBHID_TRACE (make USBHID_TRACE=1|2, one switch shared with usbkbd and
 * the kbd handler): 1 = probe steps, every report and button changes;
 * 2 = also every URB completion and resubmit.  The probe
 * result, the first report and errors always print. */
#ifndef USBHID_TRACE
#define USBHID_TRACE 0
#endif
#if USBHID_TRACE >= 1
#define MOUSE_LOG(...) printf("usbmouse: " __VA_ARGS__)
#else
#define MOUSE_LOG(...) do { } while(0)
#endif
#if USBHID_TRACE >= 2
#define MOUSE_LOG2(...) printf("usbmouse: " __VA_ARGS__)
#else
#define MOUSE_LOG2(...) do { } while(0)
#endif

#define HID_REQ_SET_IDLE     0x0a
#define HID_REQ_SET_PROTOCOL 0x0b
#define HID_BOOT_PROTOCOL    0
#define MOUSE_MIN_REPORT     3
#define MOUSE_MAX_REPORT     64
#define MAX_ERRORS           32

struct usbmouse {
  struct spinlock lock;
  struct usb_device *udev;
  struct input_dev *input;
  struct urb *irq_urb;
  struct work_struct rx_work;
  uchar report[64] __attribute__((aligned(64)));   // DMA target
  int report_len;
  int active;
  uint report_count;
  uint errors;
  uchar old_buttons;
};

static struct workqueue usbmouse_wq;

static const ushort buttons[5] = {
  BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA,
};
#if USBHID_TRACE >= 1
static const char *button_names[5] = {
  "BTN_LEFT", "BTN_RIGHT", "BTN_MIDDLE", "BTN_SIDE", "BTN_EXTRA",
};
#endif

static void
usbmouse_report(struct usbmouse *m, int len)
{
  uchar *r = m->report;

  if(m->report_count++ == 0)
    printf("usbmouse: first report len=%d buttons=%x dx=%d dy=%d\n",
           len, r[0], (signed char)r[1], (signed char)r[2]);
  else
    MOUSE_LOG("report #%d len=%d buttons=%x dx=%d dy=%d wheel=%d\n",
              m->report_count, len, r[0], (signed char)r[1],
              (signed char)r[2], len >= 4 ? (signed char)r[3] : 0);
#if USBHID_TRACE >= 1
  for(int i = 0; i < 5; i++)
    if(((r[0] ^ m->old_buttons) >> i) & 1)
      MOUSE_LOG("  %s %s\n", button_names[i], (r[0] >> i) & 1 ? "down" : "up");
#endif
  m->old_buttons = r[0];
  // Buttons are absolute state: the input core drops repeats.
  for(int i = 0; i < 5; i++)
    input_report_key(m->input, buttons[i], (r[0] >> i) & 1);
  input_report_rel(m->input, REL_X, (signed char)r[1]);
  input_report_rel(m->input, REL_Y, (signed char)r[2]);
  if(len >= 4)
    input_report_rel(m->input, REL_WHEEL, (signed char)r[3]);
  input_sync(m->input);
}

static void
usbmouse_rx_work(struct work_struct *work)
{
  struct usbmouse *m = (struct usbmouse *)((char *)work -
    __builtin_offsetof(struct usbmouse, rx_work));
  struct urb *urb = m->irq_urb;

  acquire(&m->lock);
  if(!m->active){
    release(&m->lock);
    return;
  }
  release(&m->lock);

  MOUSE_LOG2("urb done status=%d len=%d\n", urb->status, urb->actual_length);
  if(urb->status == 0 && urb->actual_length >= MOUSE_MIN_REPORT){
    m->errors = 0;
    usbmouse_report(m, urb->actual_length);
  } else if(urb->status == 0){
    // Short packet: nothing usable.
    MOUSE_LOG("short report len=%d ignored\n", urb->actual_length);
  } else if(++m->errors >= MAX_ERRORS){
    printf("usbmouse: %d transfer errors in a row; mouse stopped\n",
           m->errors);
    return;
  } else if(m->errors <= 4){
    printf("usbmouse: interrupt transfer error; resubmitting\n");
  }

  acquire(&m->lock);
  int rearm = m->active;
  release(&m->lock);
  if(rearm && usb_submit_urb(urb) < 0)
    printf("usbmouse: cannot resubmit interrupt URB\n");
  else if(rearm)
    MOUSE_LOG2("urb resubmitted ep%d len %d\n", urb->endpoint,
               urb->transfer_buffer_length);
}

// URB completion, interrupt context: defer the work.
static void
usbmouse_irq(struct urb *urb)
{
  struct usbmouse *m = urb->context;

  acquire(&m->lock);
  if(m->active)
    queue_work(&usbmouse_wq, &m->rx_work);
  release(&m->lock);
}

static int
usbmouse_probe(struct usb_device *udev)
{
  struct usbmouse *m;

  if(udev->ops == 0 || udev->ops->submit_urb == 0 ||
     udev->ops->kill_urb == 0 || udev->interrupt_in_ep == 0)
    return -1;
  _Static_assert(sizeof(struct usbmouse) <= PGSIZE, "usbmouse too large");
  m = kalloc();
  if(m == 0)
    return -1;
  memset(m, 0, sizeof(*m));
  initlock(&m->lock, "usbmouse");
  m->udev = udev;
  init_work(&m->rx_work, usbmouse_rx_work);
  m->input = input_allocate_device();
  m->irq_urb = usb_alloc_urb();
  if(m->input == 0 || m->irq_urb == 0)
    goto fail;
  MOUSE_LOG("probe %x:%x addr %d iface %d class %d/%d/%d intr-in ep%d mps %d interval %d\n",
            udev->vendor, udev->product, udev->address, udev->interface_number,
            udev->class, udev->subclass, udev->protocol, udev->interrupt_in_ep,
            udev->interrupt_in_max_packet, udev->interrupt_in_interval);

  // Boot protocol: the fixed 3-byte buttons/X/Y layout (plus the wheel
  // byte most mice keep sending).  Without it a mouse in report protocol
  // may use a different layout, so warn when it is refused.
  if(udev->ops->control(udev, 0x21, HID_REQ_SET_PROTOCOL, HID_BOOT_PROTOCOL,
                        udev->interface_number, 0, 0) < 0)
    printf("usbmouse: SET_PROTOCOL(boot) failed; assuming boot reports\n");
  else
    MOUSE_LOG("SET_PROTOCOL(boot) ok\n");
  int idle = udev->ops->control(udev, 0x21, HID_REQ_SET_IDLE, 0,
                                udev->interface_number, 0, 0);
  MOUSE_LOG("SET_IDLE(0) %s\n", idle < 0 ? "stalled (ignored)" : "ok");
  (void)idle;

  // Ask for one whole packet (Linux: usb_maxpacket), so a mouse that sends
  // more than the boot bytes does not babble.
  m->report_len = udev->interrupt_in_max_packet;
  if(m->report_len < MOUSE_MIN_REPORT)
    m->report_len = MOUSE_MIN_REPORT;
  if(m->report_len > MOUSE_MAX_REPORT)
    m->report_len = MOUSE_MAX_REPORT;

  m->input->name = "USB HID Boot Mouse";
  m->input->phys = "usb-ehci/input0";
  m->input->bustype = BUS_USB;
  m->input->vendor = udev->vendor;
  m->input->product = udev->product;
  m->input->private = m;
  for(int i = 0; i < 5; i++)
    input_set_capability(m->input, EV_KEY, buttons[i]);
  input_set_capability(m->input, EV_REL, REL_X);
  input_set_capability(m->input, EV_REL, REL_Y);
  input_set_capability(m->input, EV_REL, REL_WHEEL);
  if(input_register_device(m->input) < 0)
    goto fail;

  usb_fill_int_urb(m->irq_urb, udev, udev->interrupt_in_ep, m->report,
                   m->report_len, usbmouse_irq, m,
                   udev->interrupt_in_interval);
  m->active = 1;
  udev->dev.driver_data = m;
  if(usb_submit_urb(m->irq_urb) < 0){
    printf("usbmouse: cannot submit interrupt URB\n");
    m->active = 0;
    udev->dev.driver_data = 0;
    input_unregister_device(m->input);
    goto fail;
  }
  printf("usbmouse: %x:%x mouse on ep%d in, mps %d, interval %d\n",
         udev->vendor, udev->product, udev->interrupt_in_ep,
         udev->interrupt_in_max_packet, udev->interrupt_in_interval);
  return 0;

fail:
  usb_free_urb(m->irq_urb);
  input_free_device(m->input);
  kfree(m);
  return -1;
}

static void
usbmouse_remove(struct usb_device *udev)
{
  struct usbmouse *m = udev->dev.driver_data;

  if(m == 0)
    return;
  acquire(&m->lock);
  m->active = 0;
  release(&m->lock);
  usb_kill_urb(m->irq_urb);
  cancel_work_sync(&m->rx_work);
  input_unregister_device(m->input);   // releases buttons still held
  udev->dev.driver_data = 0;
  usb_free_urb(m->irq_urb);
  input_free_device(m->input);
  kfree(m);
}

static const struct usb_device_id usbmouse_ids[] = {
  {
    .class = 3,           // HID
    .subclass = 1,        // boot interface
    .protocol = 2,        // mouse
    .match_flags = USB_DEVICE_ID_MATCH_CLASS |
                   USB_DEVICE_ID_MATCH_SUBCLASS |
                   USB_DEVICE_ID_MATCH_PROTOCOL,
  },
  { 0 },
};

static struct usb_driver usbmouse_driver = {
  .driver = { .name = "usbmouse" },
  .id_table = usbmouse_ids,
  .probe = usbmouse_probe,
  .remove = usbmouse_remove,
};

static void
usbmouse_driver_init(void)
{
  init_workqueue(&usbmouse_wq, "usbmouse_wq", 1);
  usb_register_driver(&usbmouse_driver);
}

// Class drivers register before the EHCI host (order 2) enumerates.
module_init_ord("usbmouse", usbmouse_driver_init, 1);
