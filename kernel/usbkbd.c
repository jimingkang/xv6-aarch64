// USB HID boot-protocol keyboard driver.
//
// The DWC2 host controller supplies interrupt-IN reports.  This class driver
// translates the standard eight-byte boot report into the same character
// input path used by the Mini UART, so the existing console/TTY line discipline
// remains the single owner of echo, erase, ^C and wakeups.

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

#define HID_REQ_SET_IDLE     0x0a
#define HID_REQ_SET_PROTOCOL 0x0b
#define HID_BOOT_PROTOCOL    0
#define HID_REPORT_SIZE      8
#define HID_ERR_ROLLOVER     1   // usages 1..3: ErrorRollOver/POSTFail/Undefined
#define HID_ERR_LAST         3

struct usbkbd_state {
  struct spinlock lock;
  struct usb_device *udev;
  struct input_dev *input;
  struct urb *irq_urb;
  struct work_struct rx_work;
  uchar report[64] __attribute__((aligned(64)));
  uchar previous[HID_REPORT_SIZE];
  int active;
  int disconnected;
  int capslock;
  uint irq_count;
  uint report_count;
  uint error_count;
  int irq_announced;
};

static struct workqueue usbkbd_wq;

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
  [56]=KEY_SLASH,[57]=KEY_CAPSLOCK,[79]=KEY_RIGHT,[80]=KEY_LEFT,
  [81]=KEY_DOWN,[82]=KEY_UP,
  [224]=KEY_LEFTCTRL,[225]=KEY_LEFTSHIFT,[226]=KEY_LEFTALT,
  [227]=KEY_LEFTMETA,[228]=KEY_RIGHTCTRL,[229]=KEY_RIGHTSHIFT,
  [230]=KEY_RIGHTALT,[231]=KEY_RIGHTMETA,
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

  if(hid_to_key[code])
    input_report_key(kbd->input, hid_to_key[code], 1);
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
  int n, status, rearm;
  int announce_irq = 0;

  acquire(&kbd->lock);
  if(!kbd->active || kbd->udev == 0 || kbd->irq_urb == 0){
    release(&kbd->lock);
    return;
  }
  if(!kbd->irq_announced && kbd->irq_count){
    kbd->irq_announced = 1;
    announce_irq = 1;
  }
  release(&kbd->lock);
  if(announce_irq)
    printf("usbkbd: channel 6 IRQ received\n");
  status = kbd->irq_urb->status;
  n = kbd->irq_urb->actual_length;
  if(status < 0){
    if(kbd->error_count++ < 4)
      printf("usbkbd: channel 6 transfer error; rearming\n");
  }
  if(status == 0 && n >= HID_REPORT_SIZE){
    if(kbd->report_count++ == 0)
      printf("usbkbd: first HID report mod=%x keys=%x,%x,%x,%x,%x,%x\n",
             kbd->report[0], kbd->report[2], kbd->report[3],
             kbd->report[4], kbd->report[5], kbd->report[6],
             kbd->report[7]);
    // On phantom/rollover the keyboard fills every key slot with an error
    // usage.  The report says nothing about which keys are down, so keep the
    // previous state; treating it as "all released" would emit spurious
    // key-ups and then repeat the presses (and console chars) next report.
    if(kbd->report[2] >= HID_ERR_ROLLOVER && kbd->report[2] <= HID_ERR_LAST)
      goto rearm;
    // Modifier bits are HID usages 0xe0..0xe7 and are independent of the six
    // ordinary-key slots.
    for(int i = 0; i < 8; i++){
      int old_down = (kbd->previous[0] & (1U << i)) != 0;
      int new_down = (kbd->report[0] & (1U << i)) != 0;
      if(old_down != new_down)
        input_report_key(kbd->input, hid_to_key[224 + i], new_down);
    }
    // Usage IDs 1..3 are rollover/error reports, not keys.
    for(int i = 2; i < HID_REPORT_SIZE; i++){
      uchar code = kbd->report[i];
      if(code > 3 && !was_down(kbd, code))
        usbkbd_key(kbd, kbd->report[0], code);
    }
    for(int i = 2; i < HID_REPORT_SIZE; i++){
      uchar code = kbd->previous[i];
      int still_down = 0;
      if(code <= 3)
        continue;
      for(int j = 2; j < HID_REPORT_SIZE; j++)
        if(kbd->report[j] == code)
          still_down = 1;
      if(!still_down)
        if(hid_to_key[code])
          input_report_key(kbd->input, hid_to_key[code], 0);
    }
    input_sync(kbd->input);
    memmove(kbd->previous, kbd->report, HID_REPORT_SIZE);
  }
rearm:
  acquire(&kbd->lock);
  rearm = kbd->active && !kbd->disconnected;
  release(&kbd->lock);
  if(rearm && usb_submit_urb(kbd->irq_urb) < 0){
    acquire(&kbd->lock);
    if(kbd->active && kbd->error_count++ < 4)
      printf("usbkbd: cannot resubmit interrupt URB\n");
    release(&kbd->lock);
  }
}

// Linux-style URB completion.  DWC2 has returned ownership of the request;
// keep the callback short and defer HID/input processing to the device's work.
static void
usbkbd_irq_complete(struct urb *urb)
{
  struct usbkbd_state *kbd = urb ? urb->context : 0;
  if(kbd == 0)
    return;
  acquire(&kbd->lock);
  if(kbd->active && !kbd->disconnected){
    kbd->irq_count++;
    queue_work(&usbkbd_wq, &kbd->rx_work);
  }
  release(&kbd->lock);
}

static int
usbkbd_probe(struct usb_device *udev)
{
  struct usbkbd_state *kbd;

  if(udev == 0 || udev->ops == 0 || udev->ops->submit_urb == 0 ||
     udev->ops->kill_urb == 0 ||
     udev->class != 3 || udev->subclass != 1 || udev->protocol != 1 ||
     udev->interrupt_in_ep == 0)
    return -1;
  kbd = kalloc();
  if(kbd == 0)
    return -1;
  memset(kbd, 0, sizeof(*kbd));
  initlock(&kbd->lock, "usbkbd");
  kbd->udev = udev;
  kbd->input = input_allocate_device();
  if(kbd->input == 0){
    kfree(kbd);
    return -1;
  }
  kbd->irq_urb = usb_alloc_urb();
  if(kbd->irq_urb == 0){
    input_free_device(kbd->input);
    kfree(kbd);
    return -1;
  }
  init_work(&kbd->rx_work, usbkbd_rx_work);
  // Force the compact, universally specified 8-byte keyboard report format.
  if(udev->ops->control(udev, 0x21, HID_REQ_SET_PROTOCOL,
                        HID_BOOT_PROTOCOL, udev->interface_number, 0, 0) < 0)
    goto fail;
  // Idle duration zero: send reports only when state changes.
  udev->ops->control(udev, 0x21, HID_REQ_SET_IDLE, 0,
                     udev->interface_number, 0, 0);
  kbd->input->name = "USB HID Boot Keyboard";
  kbd->input->phys = "usb/dwc2/input0";
  kbd->input->bustype = 3; // BUS_USB
  kbd->input->vendor = udev->vendor;
  kbd->input->product = udev->product;
  kbd->input->private = kbd;
  for(int i = 0; i < 256; i++)
    if(hid_to_key[i])
      input_set_capability(kbd->input, EV_KEY, hid_to_key[i]);
  if(input_register_device(kbd->input) < 0)
    goto fail;
  usb_fill_int_urb(kbd->irq_urb, udev, udev->interrupt_in_ep,
                   kbd->report, HID_REPORT_SIZE, usbkbd_irq_complete, kbd,
                   udev->interrupt_in_interval);
  kbd->active = 1;
  udev->dev.driver_data = kbd;
  if(usb_submit_urb(kbd->irq_urb) < 0)
    goto fail_registered;
  printf("usbkbd: IRQ-driven boot keyboard ready ep=%d mps=%d interval=%d\n",
         udev->interrupt_in_ep, udev->interrupt_in_max_packet,
         udev->interrupt_in_interval);
  return 0;

fail_registered:
  udev->dev.driver_data = 0;
  kbd->active = 0;
  input_unregister_device(kbd->input);
fail:
  usb_free_urb(kbd->irq_urb);
  input_free_device(kbd->input);
  kfree(kbd);
  return -1;
}

static void
usbkbd_remove(struct usb_device *udev)
{
  struct usbkbd_state *kbd = udev ? udev->dev.driver_data : 0;
  if(kbd == 0)
    return;
  acquire(&kbd->lock);
  kbd->active = 0;
  kbd->disconnected = 1;
  release(&kbd->lock);

  // Stop DMA/channel completion first.  When this returns no HCD completion
  // can queue new class-driver work for this device.
  usb_kill_urb(kbd->irq_urb);
  cancel_work_sync(&kbd->rx_work);

  input_unregister_device(kbd->input);
  udev->dev.driver_data = 0;
  usb_free_urb(kbd->irq_urb);
  input_free_device(kbd->input);
  kfree(kbd);
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
