// USB HID boot-protocol keyboard driver.
//
// The DWC2 host controller supplies interrupt-IN reports.  This class driver
// translates the standard eight-byte boot report into the same character
// input path used by the Mini UART, so the existing console/TTY line discipline
// remains the single owner of echo, erase, ^C and wakeups.

#include "types.h"
#include "aarch64.h"
#include "spinlock.h"
#include "defs.h"
#include "device.h"
#include "usb.h"
#include "workqueue.h"

#define HID_REQ_SET_IDLE     0x0a
#define HID_REQ_SET_PROTOCOL 0x0b
#define HID_BOOT_PROTOCOL    0
#define HID_REPORT_SIZE      8

struct usbkbd_state {
  struct spinlock lock;
  struct usb_device *udev;
  struct work_struct rx_work;
  uchar report[64] __attribute__((aligned(64)));
  uchar previous[HID_REPORT_SIZE];
  int active;
  int capslock;
};

static struct workqueue usbkbd_wq;
static struct usbkbd_state keyboard;

static const uchar keymap[58] = {
  [4]='a',[5]='b',[6]='c',[7]='d',[8]='e',[9]='f',[10]='g',[11]='h',
  [12]='i',[13]='j',[14]='k',[15]='l',[16]='m',[17]='n',[18]='o',
  [19]='p',[20]='q',[21]='r',[22]='s',[23]='t',[24]='u',[25]='v',
  [26]='w',[27]='x',[28]='y',[29]='z',
  [30]='1',[31]='2',[32]='3',[33]='4',[34]='5',[35]='6',[36]='7',
  [37]='8',[38]='9',[39]='0',[40]='\n',[41]=0x1b,[42]='\b',[43]='\t',
  [44]=' ',[45]='-',[46]='=',[47]='[',[48]=']',[49]='\\',[51]=';',
  [52]='\'',[53]='`',[54]=',',[55]='.',[56]='/',
};

static const uchar shiftmap[58] = {
  [4]='A',[5]='B',[6]='C',[7]='D',[8]='E',[9]='F',[10]='G',[11]='H',
  [12]='I',[13]='J',[14]='K',[15]='L',[16]='M',[17]='N',[18]='O',
  [19]='P',[20]='Q',[21]='R',[22]='S',[23]='T',[24]='U',[25]='V',
  [26]='W',[27]='X',[28]='Y',[29]='Z',
  [30]='!',[31]='@',[32]='#',[33]='$',[34]='%',[35]='^',[36]='&',
  [37]='*',[38]='(',[39]=')',[40]='\n',[41]=0x1b,[42]='\b',[43]='\t',
  [44]=' ',[45]='_',[46]='+',[47]='{',[48]='}',[49]='|',[51]=':',
  [52]='"',[53]='~',[54]='<',[55]='>',[56]='?',
};

static int
was_down(struct usbkbd_state *kbd, uchar code)
{
  for(int i = 2; i < HID_REPORT_SIZE; i++)
    if(kbd->previous[i] == code)
      return 1;
  return 0;
}

static void
emit_escape(char final)
{
  consoleintr(0x1b);
  consoleintr('[');
  consoleintr(final);
}

static void
usbkbd_key(struct usbkbd_state *kbd, uchar modifiers, uchar code)
{
  int shifted = (modifiers & ((1U << 1) | (1U << 5))) != 0;
  int control = (modifiers & ((1U << 0) | (1U << 4))) != 0;
  int c;

  if(code == 57){
    kbd->capslock ^= 1;
    return;
  }
  if(code == 79){ emit_escape('C'); return; }
  if(code == 80){ emit_escape('D'); return; }
  if(code == 81){ emit_escape('B'); return; }
  if(code == 82){ emit_escape('A'); return; }
  if(code >= sizeof(keymap) || keymap[code] == 0)
    return;
  if(code >= 4 && code <= 29)
    shifted ^= kbd->capslock;
  c = shifted ? shiftmap[code] : keymap[code];
  if(control && c >= 'a' && c <= 'z')
    c = c - 'a' + 1;
  else if(control && c >= 'A' && c <= 'Z')
    c = c - 'A' + 1;
  consoleintr(c);
}

static void
usbkbd_rx_work(struct work_struct *work)
{
  struct usbkbd_state *kbd =
    (struct usbkbd_state *)((char *)work -
      __builtin_offsetof(struct usbkbd_state, rx_work));
  int n;

  acquire(&kbd->lock);
  if(!kbd->active || kbd->udev == 0 ||
     kbd->udev->ops->interrupt_rx_complete == 0){
    release(&kbd->lock);
    return;
  }
  release(&kbd->lock);
  n = kbd->udev->ops->interrupt_rx_complete(
        kbd->udev, kbd->udev->interrupt_in_ep);
  // The HCD has already rearmed an intermediate start/complete split or a
  // NAK transaction.  Wait for the next real channel-6 IRQ.
  if(n == -2)
    return;
  if(n >= HID_REPORT_SIZE){
    // Usage IDs 1..3 are rollover/error reports, not keys.
    for(int i = 2; i < HID_REPORT_SIZE; i++){
      uchar code = kbd->report[i];
      if(code > 3 && !was_down(kbd, code))
        usbkbd_key(kbd, kbd->report[0], code);
    }
    memmove(kbd->previous, kbd->report, HID_REPORT_SIZE);
  }
  acquire(&kbd->lock);
  if(kbd->active)
    kbd->udev->ops->interrupt_rx_arm(kbd->udev,
                                     kbd->udev->interrupt_in_ep,
                                     kbd->report, HID_REPORT_SIZE);
  release(&kbd->lock);
}

// Called from the DWC2 IRQ top half after channel 6 has been masked.  The
// workqueue coalesces duplicate notifications and performs all cache/HID work
// in schedulable process context.
void
usbkbd_rx_irq(void)
{
  acquire(&keyboard.lock);
  if(keyboard.active)
    queue_work(&usbkbd_wq, &keyboard.rx_work);
  release(&keyboard.lock);
}

static int
usbkbd_probe(struct usb_device *udev)
{
  if(udev == 0 || udev->ops == 0 ||
     udev->ops->interrupt_rx_arm == 0 ||
     udev->ops->interrupt_rx_complete == 0 ||
     udev->class != 3 || udev->subclass != 1 || udev->protocol != 1 ||
     udev->interrupt_in_ep == 0)
    return -1;
  memset(&keyboard, 0, sizeof(keyboard));
  initlock(&keyboard.lock, "usbkbd");
  keyboard.udev = udev;
  init_work(&keyboard.rx_work, usbkbd_rx_work);
  // Force the compact, universally specified 8-byte keyboard report format.
  if(udev->ops->control(udev, 0x21, HID_REQ_SET_PROTOCOL,
                        HID_BOOT_PROTOCOL, udev->interface_number, 0, 0) < 0)
    return -1;
  // Idle duration zero: send reports only when state changes.
  udev->ops->control(udev, 0x21, HID_REQ_SET_IDLE, 0,
                     udev->interface_number, 0, 0);
  keyboard.active = 1;
  udev->dev.driver_data = &keyboard;
  if(udev->ops->interrupt_rx_arm(udev, udev->interrupt_in_ep,
                                 keyboard.report, HID_REPORT_SIZE) < 0){
    keyboard.active = 0;
    udev->dev.driver_data = 0;
    return -1;
  }
  printf("usbkbd: IRQ-driven boot keyboard ready ep=%d mps=%d interval=%d\n",
         udev->interrupt_in_ep, udev->interrupt_in_max_packet,
         udev->interrupt_in_interval);
  return 0;
}

static void
usbkbd_remove(struct usb_device *udev)
{
  acquire(&keyboard.lock);
  keyboard.active = 0;
  release(&keyboard.lock);
  cancel_work_sync(&keyboard.rx_work);
  if(udev)
    udev->dev.driver_data = 0;
  memset(&keyboard, 0, sizeof(keyboard));
}

static const struct usb_device_id usbkbd_ids[] = {
  {
    .class = 3,
    .subclass = 1,
    .protocol = 1,
    .match_flags = USB_DEVICE_ID_MATCH_CLASS |
                   USB_DEVICE_ID_MATCH_SUBCLASS |
                   USB_DEVICE_ID_MATCH_PROTOCOL,
  },
  { 0 },
};

static struct usb_driver usbkbd_driver = {
  .driver = { .name = "usbhid-keyboard" },
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
