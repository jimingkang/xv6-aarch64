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
#include "usb.h"
#include "workqueue.h"

#define HID_REQ_SET_IDLE     0x0a
#define HID_REQ_SET_PROTOCOL 0x0b
#define HID_BOOT_PROTOCOL    0
#define HID_REPORT_SIZE      8
#define USBKBD_EVENTS        64

// Minimal Linux input_event-like payload without timestamps.  type=1 means
// EV_KEY; value=1 is press and value=0 is release.
struct usbkbd_event {
  uint16 type;
  uint16 code;
  int value;
};

struct usbkbd_state {
  struct spinlock lock;
  struct usb_device *udev;
  struct urb *irq_urb;
  struct work_struct rx_work;
  uchar report[64] __attribute__((aligned(64)));
  uchar previous[HID_REPORT_SIZE];
  int active;
  int disconnected;
  int readers;
  int capslock;
  struct usbkbd_event events[USBKBD_EVENTS];
  uint event_r;
  uint event_w;
  uint irq_count;
  uint report_count;
  uint error_count;
  int irq_announced;
};

static struct workqueue usbkbd_wq;
static struct spinlock usbkbd_devices_lock;
static struct usbkbd_state *primary_keyboard;

static void
usbkbd_event(struct usbkbd_state *kbd, uchar code, int value)
{
  struct usbkbd_event *ev;
  acquire(&kbd->lock);
  if(kbd->event_w - kbd->event_r == USBKBD_EVENTS)
    kbd->event_r++; // keep the newest events if userspace is too slow
  ev = &kbd->events[kbd->event_w++ % USBKBD_EVENTS];
  ev->type = 1;
  ev->code = code;
  ev->value = value;
  wakeup(&kbd->event_r);
  release(&kbd->lock);
}

static int
usbkbd_event_read(int user_dst, uint64 dst, int n)
{
  int copied = 0;
  int result = 0;
  struct usbkbd_event ev;
  struct usbkbd_state *kbd;

  if(n < (int)sizeof(ev))
    return -1;
  acquire(&usbkbd_devices_lock);
  kbd = primary_keyboard;
  if(kbd == 0){
    release(&usbkbd_devices_lock);
    return -1;
  }
  acquire(&kbd->lock);
  kbd->readers++;
  release(&usbkbd_devices_lock);
  while(kbd->event_r == kbd->event_w && !kbd->disconnected){
    if(myproc()->killed){
      result = -1;
      goto out;
    }
    sleep(&kbd->event_r, &kbd->lock);
  }
  if(kbd->disconnected){
    result = -1;
    goto out;
  }
  while(n - copied >= (int)sizeof(ev) &&
        kbd->event_r != kbd->event_w){
    ev = kbd->events[kbd->event_r++ % USBKBD_EVENTS];
    release(&kbd->lock);
    if(either_copyout(user_dst, dst + copied, &ev, sizeof(ev)) < 0)
      result = copied ? copied : -1;
    else
      copied += sizeof(ev);
    acquire(&kbd->lock);
    if(result)
      goto out;
  }
  result = copied;
out:
  kbd->readers--;
  wakeup(&kbd->readers);
  release(&kbd->lock);
  return result;
}

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

  usbkbd_event(kbd, code, 1);
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
        usbkbd_event(kbd, code, 0);
    }
    memmove(kbd->previous, kbd->report, HID_REPORT_SIZE);
  }
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
  kbd->irq_urb = usb_alloc_urb();
  if(kbd->irq_urb == 0){
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
  usb_fill_int_urb(kbd->irq_urb, udev, udev->interrupt_in_ep,
                   kbd->report, HID_REPORT_SIZE, usbkbd_irq_complete, kbd,
                   udev->interrupt_in_interval);
  kbd->active = 1;
  udev->dev.driver_data = kbd;
  acquire(&usbkbd_devices_lock);
  if(primary_keyboard == 0)
    primary_keyboard = kbd;
  release(&usbkbd_devices_lock);
  if(usb_submit_urb(kbd->irq_urb) < 0)
    goto fail_registered;
  printf("usbkbd: IRQ-driven boot keyboard ready ep=%d mps=%d interval=%d\n",
         udev->interrupt_in_ep, udev->interrupt_in_max_packet,
         udev->interrupt_in_interval);
  return 0;

fail_registered:
  acquire(&usbkbd_devices_lock);
  if(primary_keyboard == kbd)
    primary_keyboard = 0;
  release(&usbkbd_devices_lock);
  udev->dev.driver_data = 0;
  kbd->active = 0;
fail:
  usb_free_urb(kbd->irq_urb);
  kfree(kbd);
  return -1;
}

static void
usbkbd_remove(struct usb_device *udev)
{
  struct usbkbd_state *kbd = udev ? udev->dev.driver_data : 0;
  if(kbd == 0)
    return;
  acquire(&usbkbd_devices_lock);
  if(primary_keyboard == kbd)
    primary_keyboard = 0;
  acquire(&kbd->lock);
  kbd->active = 0;
  kbd->disconnected = 1;
  wakeup(&kbd->event_r);
  release(&kbd->lock);
  release(&usbkbd_devices_lock);

  // Stop DMA/channel completion first.  When this returns no HCD completion
  // can queue new class-driver work for this device.
  usb_kill_urb(kbd->irq_urb);
  cancel_work_sync(&kbd->rx_work);

  acquire(&kbd->lock);
  while(kbd->readers != 0)
    sleep(&kbd->readers, &kbd->lock);
  release(&kbd->lock);
  udev->dev.driver_data = 0;
  usb_free_urb(kbd->irq_urb);
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
  static struct file_operations event_fops = {
    .read = usbkbd_event_read,
  };
  initlock(&usbkbd_devices_lock, "usbkbd-devices");
  primary_keyboard = 0;
  register_chrdev(USBKBD, "input/event0", &event_fops);
  init_workqueue(&usbkbd_wq, "usbkbd_wq", 1);
  usb_register_driver(&usbkbd_driver);
}

void
usbkbd_driver_exit(void)
{
  usb_unregister_driver(&usbkbd_driver);
}
