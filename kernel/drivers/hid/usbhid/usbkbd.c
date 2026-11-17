/* USB HID boot-protocol keyboard driver (Linux drivers/hid/usbhid/usbkbd.c).
 *
 * Binds to interface class 3 (HID), subclass 1 (boot), protocol 1
 * (keyboard).  The EHCI host polls the keyboard's interrupt-IN endpoint on
 * the periodic schedule (split transactions through the board's USB2514
 * hub for full/low-speed keyboards) and gives each 8-byte boot report back
 * through the URB completion:
 *
 *   ehci_irq -> usb_hcd_giveback_urb -> usbkbd_irq     (interrupt context)
 *     -> queue_work(usbkbd_wq)
 *   usbkbd_rx_work: diff the report against the previous one
 *     -> input_report_key()/input_sync()  (input core: evdev, kbd handler)
 *     -> usb_submit_urb() again
 *
 * This driver only reports key presses and releases; turning them into
 * console characters is the kbd input handler's job
 * (drivers/tty/vt/keyboard.c), as in Linux.
 *
 * Boot report: byte 0 modifier bits (HID usages 0xe0..0xe7), byte 1
 * reserved, bytes 2..7 up to six pressed key usages (page 7).
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

/* USBHID_TRACE (make USBHID_TRACE=1|2, one switch shared with usbmouse and
 * the kbd handler): 1 = probe, reports, key changes, LEDs; 2 = also every
 * URB completion and resubmit.  Errors always print. */
#ifndef USBHID_TRACE
#define USBHID_TRACE 0
#endif
#if USBHID_TRACE >= 1
#define KBD_LOG(...) printf("usbkbd: " __VA_ARGS__)
#else
#define KBD_LOG(...) do { } while(0)
#endif
#if USBHID_TRACE >= 2
#define KBD_LOG2(...) printf("usbkbd: " __VA_ARGS__)
#else
#define KBD_LOG2(...) do { } while(0)
#endif

#define HID_REQ_SET_REPORT   0x09
#define HID_REQ_SET_IDLE     0x0a
#define HID_REQ_SET_PROTOCOL 0x0b
#define HID_BOOT_PROTOCOL    0
#define HID_OUTPUT_REPORT    2
#define HID_REPORT_SIZE      8
#define HID_ERR_ROLLOVER     1   // usages 1..3: ErrorRollOver/POSTFail/Undefined
#define HID_ERR_LAST         3
#define HID_USAGE_CAPSLOCK   57
#define LED_CAPSLOCK         (1U << 1)
#define MAX_ERRORS           32  // consecutive failed reports before giving up

struct usbkbd {
  struct spinlock lock;
  struct usb_device *udev;
  struct input_dev *input;
  struct urb *irq_urb;
  struct work_struct rx_work;
  uchar report[64] __attribute__((aligned(64)));   // DMA target
  uchar previous[HID_REPORT_SIZE];
  uchar leds;
  int active;
  uint report_count;
  uint errors;              // consecutive
};

static struct workqueue usbkbd_wq;

/* HID usage page 7 (keyboard) -> Linux KEY_* code (Linux hid_keyboard[]). */
static const ushort hid_to_key[256] = {
  [4]=KEY_A,[5]=KEY_B,[6]=KEY_C,[7]=KEY_D,[8]=KEY_E,[9]=KEY_F,
  [10]=KEY_G,[11]=KEY_H,[12]=KEY_I,[13]=KEY_J,[14]=KEY_K,
  [15]=KEY_L,[16]=KEY_M,[17]=KEY_N,[18]=KEY_O,[19]=KEY_P,
  [20]=KEY_Q,[21]=KEY_R,[22]=KEY_S,[23]=KEY_T,[24]=KEY_U,
  [25]=KEY_V,[26]=KEY_W,[27]=KEY_X,[28]=KEY_Y,[29]=KEY_Z,
  [30]=KEY_1,[31]=KEY_2,[32]=KEY_3,[33]=KEY_4,[34]=KEY_5,
  [35]=KEY_6,[36]=KEY_7,[37]=KEY_8,[38]=KEY_9,[39]=KEY_0,
  [40]=KEY_ENTER,[41]=KEY_ESC,[42]=KEY_BACKSPACE,[43]=KEY_TAB,
  [44]=KEY_SPACE,[45]=KEY_MINUS,[46]=KEY_EQUAL,[47]=KEY_LEFTBRACE,
  [48]=KEY_RIGHTBRACE,[49]=KEY_BACKSLASH,[51]=KEY_SEMICOLON,
  [52]=KEY_APOSTROPHE,[53]=KEY_GRAVE,[54]=KEY_COMMA,[55]=KEY_DOT,
  [56]=KEY_SLASH,[57]=KEY_CAPSLOCK,
  [58]=KEY_F1,[59]=KEY_F2,[60]=KEY_F3,[61]=KEY_F4,[62]=KEY_F5,
  [63]=KEY_F6,[64]=KEY_F7,[65]=KEY_F8,[66]=KEY_F9,[67]=KEY_F10,
  [68]=KEY_F11,[69]=KEY_F12,
  [73]=KEY_INSERT,[74]=KEY_HOME,[75]=KEY_PAGEUP,[76]=KEY_DELETE,
  [77]=KEY_END,[78]=KEY_PAGEDOWN,
  [79]=KEY_RIGHT,[80]=KEY_LEFT,[81]=KEY_DOWN,[82]=KEY_UP,
  [224]=KEY_LEFTCTRL,[225]=KEY_LEFTSHIFT,[226]=KEY_LEFTALT,
  [227]=KEY_LEFTMETA,[228]=KEY_RIGHTCTRL,[229]=KEY_RIGHTSHIFT,
  [230]=KEY_RIGHTALT,[231]=KEY_RIGHTMETA,
};

static int
in_report(uchar *r, uchar usage)
{
  for(int i = 2; i < HID_REPORT_SIZE; i++)
    if(r[i] == usage)
      return 1;
  return 0;
}

// Caps Lock LED: one-byte output report through EP0 (SET_REPORT).  Runs in
// the work item, where a synchronous control transfer may sleep.
static void
usbkbd_set_leds(struct usbkbd *kbd)
{
  uchar leds = kbd->leds;
  KBD_LOG("SET_REPORT(output) leds=%x (caps=%d)\n", leds, (leds & LED_CAPSLOCK) != 0);
  kbd->udev->ops->control(kbd->udev, 0x21, HID_REQ_SET_REPORT,
                          HID_OUTPUT_REPORT << 8,
                          kbd->udev->interface_number, &leds, 1);
}

static void
usbkbd_report(struct usbkbd *kbd)
{
  uchar *r = kbd->report, *old = kbd->previous;
  int leds_changed = 0;

  if(kbd->report_count++ == 0)
    printf("usbkbd: first report mod=%x keys=%x,%x,%x,%x,%x,%x\n",
           r[0], r[2], r[3], r[4], r[5], r[6], r[7]);
  else
    KBD_LOG("report #%d mod=%x keys=%x,%x,%x,%x,%x,%x\n", kbd->report_count,
            r[0], r[2], r[3], r[4], r[5], r[6], r[7]);
  // On phantom/rollover the keyboard fills every key slot with an error
  // usage.  It says nothing about which keys are down, so keep the previous
  // state rather than report every key released and pressed again.
  if(r[2] >= HID_ERR_ROLLOVER && r[2] <= HID_ERR_LAST){
    KBD_LOG("rollover/error report (usage 0x%x); state kept\n", r[2]);
    return;
  }
  // Modifier bits are usages 0xe0..0xe7, independent of the six key slots.
  for(int i = 0; i < 8; i++)
    if(((old[0] ^ r[0]) >> i) & 1){
      KBD_LOG("  modifier usage 0x%x -> KEY %d %s\n", 0xe0 + i,
              hid_to_key[224 + i], ((r[0] >> i) & 1) ? "down" : "up");
      input_report_key(kbd->input, hid_to_key[224 + i], (r[0] >> i) & 1);
    }
  // Releases first, then presses, like Linux usb_kbd_irq().
  for(int i = 2; i < HID_REPORT_SIZE; i++)
    if(old[i] > HID_ERR_LAST && !in_report(r, old[i]) && hid_to_key[old[i]]){
      KBD_LOG("  usage 0x%x -> KEY %d up\n", old[i], hid_to_key[old[i]]);
      input_report_key(kbd->input, hid_to_key[old[i]], 0);
    }
  for(int i = 2; i < HID_REPORT_SIZE; i++)
    if(r[i] > HID_ERR_LAST && !in_report(old, r[i])){
      if(hid_to_key[r[i]]){
        KBD_LOG("  usage 0x%x -> KEY %d down\n", r[i], hid_to_key[r[i]]);
        input_report_key(kbd->input, hid_to_key[r[i]], 1);
      }
      else
        printf("usbkbd: unmapped HID usage %d\n", r[i]);
      if(r[i] == HID_USAGE_CAPSLOCK){
        kbd->leds ^= LED_CAPSLOCK;
        leds_changed = 1;
      }
    }
  input_sync(kbd->input);
  memmove(old, r, HID_REPORT_SIZE);
  if(leds_changed)
    usbkbd_set_leds(kbd);
}

static void
usbkbd_rx_work(struct work_struct *work)
{
  struct usbkbd *kbd = (struct usbkbd *)((char *)work -
    __builtin_offsetof(struct usbkbd, rx_work));
  struct urb *urb = kbd->irq_urb;

  acquire(&kbd->lock);
  if(!kbd->active){
    release(&kbd->lock);
    return;
  }
  release(&kbd->lock);

  KBD_LOG2("urb done status=%d len=%d\n", urb->status, urb->actual_length);
  if(urb->status == 0 && urb->actual_length >= HID_REPORT_SIZE){
    kbd->errors = 0;
    usbkbd_report(kbd);
  } else if(urb->status == 0){
    // A short packet (some keyboards send less when idle); ignore it.
  } else if(++kbd->errors >= MAX_ERRORS){
    printf("usbkbd: %d transfer errors in a row; keyboard stopped\n",
           kbd->errors);
    return;
  } else if(kbd->errors <= 4){
    printf("usbkbd: interrupt transfer error; resubmitting\n");
  }

  acquire(&kbd->lock);
  int rearm = kbd->active;
  release(&kbd->lock);
  if(rearm && usb_submit_urb(urb) < 0)
    printf("usbkbd: cannot resubmit interrupt URB\n");
  else if(rearm)
    KBD_LOG2("urb resubmitted ep%d len %d\n", urb->endpoint,
             urb->transfer_buffer_length);
}

// URB completion, in interrupt context: the host has given the request
// back.  Keep it short and do the HID/input work in the work item.
static void
usbkbd_irq(struct urb *urb)
{
  struct usbkbd *kbd = urb->context;

  acquire(&kbd->lock);
  if(kbd->active)
    queue_work(&usbkbd_wq, &kbd->rx_work);
  release(&kbd->lock);
}

static int
usbkbd_probe(struct usb_device *udev)
{
  struct usbkbd *kbd;

  KBD_LOG("probe %x:%x addr %d iface %d class %d/%d/%d intr-in ep%d mps %d interval %d\n",
          udev->vendor, udev->product, udev->address, udev->interface_number,
          udev->class, udev->subclass, udev->protocol, udev->interrupt_in_ep,
          udev->interrupt_in_max_packet, udev->interrupt_in_interval);
  if(udev->ops == 0 || udev->ops->submit_urb == 0 ||
     udev->ops->kill_urb == 0 || udev->interrupt_in_ep == 0){
    printf("usbkbd: %x:%x has no interrupt-IN endpoint or URB support\n",
           udev->vendor, udev->product);
    return -1;
  }
  _Static_assert(sizeof(struct usbkbd) <= PGSIZE, "usbkbd too large");
  kbd = kalloc();
  if(kbd == 0)
    return -1;
  memset(kbd, 0, sizeof(*kbd));
  initlock(&kbd->lock, "usbkbd");
  kbd->udev = udev;
  init_work(&kbd->rx_work, usbkbd_rx_work);
  kbd->input = input_allocate_device();
  kbd->irq_urb = usb_alloc_urb();
  if(kbd->input == 0 || kbd->irq_urb == 0)
    goto fail;

  // Ask for the fixed 8-byte boot report; boot devices must accept it, and
  // their report-protocol report is the same layout anyway.
  if(udev->ops->control(udev, 0x21, HID_REQ_SET_PROTOCOL, HID_BOOT_PROTOCOL,
                        udev->interface_number, 0, 0) < 0)
    printf("usbkbd: SET_PROTOCOL(boot) failed; assuming boot reports\n");
  else
    KBD_LOG("SET_PROTOCOL(boot) ok\n");
  // Idle rate 0: report only when something changes (may STALL; harmless).
  int idle = udev->ops->control(udev, 0x21, HID_REQ_SET_IDLE, 0,
                                udev->interface_number, 0, 0);
  KBD_LOG("SET_IDLE(0) %s\n", idle < 0 ? "stalled (ignored)" : "ok");
  (void)idle;
  usbkbd_set_leds(kbd);                    // all LEDs off

  kbd->input->name = "USB HID Boot Keyboard";
  kbd->input->phys = "usb-ehci/input0";
  kbd->input->bustype = BUS_USB;
  kbd->input->vendor = udev->vendor;
  kbd->input->product = udev->product;
  kbd->input->private = kbd;
  for(int i = 0; i < 256; i++)
    if(hid_to_key[i])
      input_set_capability(kbd->input, EV_KEY, hid_to_key[i]);
  if(input_register_device(kbd->input) < 0)
    goto fail;

  usb_fill_int_urb(kbd->irq_urb, udev, udev->interrupt_in_ep, kbd->report,
                   HID_REPORT_SIZE, usbkbd_irq, kbd,
                   udev->interrupt_in_interval);
  kbd->active = 1;
  udev->dev.driver_data = kbd;
  if(usb_submit_urb(kbd->irq_urb) < 0){
    printf("usbkbd: cannot submit interrupt URB\n");
    kbd->active = 0;
    udev->dev.driver_data = 0;
    input_unregister_device(kbd->input);
    goto fail;
  }
  printf("usbkbd: %x:%x keyboard on ep%d in, mps %d, interval %d\n",
         udev->vendor, udev->product, udev->interrupt_in_ep,
         udev->interrupt_in_max_packet, udev->interrupt_in_interval);
  return 0;

fail:
  usb_free_urb(kbd->irq_urb);
  input_free_device(kbd->input);
  kfree(kbd);
  return -1;
}

static void
usbkbd_remove(struct usb_device *udev)
{
  struct usbkbd *kbd = udev->dev.driver_data;

  if(kbd == 0)
    return;
  acquire(&kbd->lock);
  kbd->active = 0;
  release(&kbd->lock);
  // No completion can queue the work once the URB is killed.
  usb_kill_urb(kbd->irq_urb);
  cancel_work_sync(&kbd->rx_work);
  input_unregister_device(kbd->input);   // releases keys still held
  udev->dev.driver_data = 0;
  usb_free_urb(kbd->irq_urb);
  input_free_device(kbd->input);
  kfree(kbd);
}

static const struct usb_device_id usbkbd_ids[] = {
  {
    .class = 3,           // HID
    .subclass = 1,        // boot interface
    .protocol = 1,        // keyboard
    .match_flags = USB_DEVICE_ID_MATCH_CLASS |
                   USB_DEVICE_ID_MATCH_SUBCLASS |
                   USB_DEVICE_ID_MATCH_PROTOCOL,
  },
  { 0 },
};

static struct usb_driver usbkbd_driver = {
  .driver = { .name = "usbkbd" },
  .id_table = usbkbd_ids,
  .probe = usbkbd_probe,
  .remove = usbkbd_remove,
};

void
usbkbd_driver_init(void)
{
  init_workqueue(&usbkbd_wq, "usbkbd_wq", 1);
  usb_register_driver(&usbkbd_driver);
}

void
usbkbd_driver_exit(void)
{
  usb_unregister_driver(&usbkbd_driver);
}

// Class drivers register before the EHCI host (order 2) enumerates.
module_init_ord("usbkbd", usbkbd_driver_init, 1);
