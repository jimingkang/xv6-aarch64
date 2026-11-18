// Minimal Linux-style input core and evdev bridge.
//
// Device drivers report state changes through input_report_key()/input_sync().
// The input core keeps the current key state and fans events out to
//   - every open file of the device's /dev/input/eventN (character major
//     INPUT, minor N = the slot the device registered in).  Like Linux evdev,
//     each open file gets its own buffer, so readers do not steal events from
//     one another and never see events from before they opened the device;
//   - the in-kernel handlers connected to the device (input_register_handler,
//     e.g. the console keyboard handler in drivers/tty/vt/keyboard.c);
//   - the win32k window manager's raw queue, when it is linked in.
//
//   driver (gpio-keys, usbkbd) -> input core -> evdev clients / handlers

#include "types.h"
#include "param.h"
#include "aarch64.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "proc.h"
#include "file.h"
#include "fcntl.h"
#include "defs.h"
#include "device.h"
#include "input.h"
#include "module.h"

void win32k_input_event(uint16, uint16, int) __attribute__((weak));

// Handler connections share the driver-model switch (kernel/device.c).
#ifndef DRIVER_CORE_TRACE
#define DRIVER_CORE_TRACE 0
#endif
#if DRIVER_CORE_TRACE >= 1
#define IN_LOG(...) printf("dm: input: " __VA_ARGS__)
#else
#define IN_LOG(...) do { } while(0)
#endif

// Both live in a single kalloc() page.
_Static_assert(sizeof(struct input_dev) <= PGSIZE, "input_dev too large");
_Static_assert(sizeof(struct evdev_client) <= PGSIZE, "evdev_client too large");
_Static_assert((EVDEV_BUFFER & (EVDEV_BUFFER - 1)) == 0,
               "EVDEV_BUFFER must be a power of two");

// Lock order: input_devices_lock, then input_dev.lock.
// input_devices_lock protects devices[] and handlers[].
static struct spinlock input_devices_lock;
static struct input_dev *devices[INPUT_MAX_DEVICES];   // index = eventN
static struct input_handler *handlers[INPUT_MAX_HANDLERS];

static void
input_put_device(struct input_dev *dev)
{
  int last;

  acquire(&dev->lock);
  if(dev->refs <= 0)
    panic("input_put_device");
  last = --dev->refs == 0;
  release(&dev->lock);
  if(last)
    kfree(dev);
}

// Caller holds dev->lock.
static void
evdev_push(struct evdev_client *client, uint16 type, uint16 code, int value)
{
  struct input_event *ev = &client->buffer[client->head++ % EVDEV_BUFFER];
  ev->type = type;
  ev->code = code;
  ev->value = value;
}

// Caller holds dev->lock.
static void
evdev_pass_event(struct evdev_client *client, uint16 type, uint16 code,
                 int value)
{
  if(client->head - client->tail >= EVDEV_BUFFER - 1){
    // This reader fell behind.  Like Linux, discard its backlog and tell it
    // with SYN_DROPPED; it must ignore events up to the next SYN_REPORT.
    // SYN_DROPPED becomes readable together with that next complete frame.
    client->tail = client->head;
    client->packet_head = client->tail;
    evdev_push(client, EV_SYN, SYN_DROPPED, 0);
  }
  evdev_push(client, type, code, value);
  // One HID report is one frame.  Publish and wake the reader only when the
  // frame is complete, so read() never returns half a frame (Linux evdev).
  if(type == EV_SYN && code == SYN_REPORT){
    client->packet_head = client->head;
    wakeup(client);
  }
}

// Caller holds dev->lock.
static void
input_queue_locked(struct input_dev *dev, uint16 type, uint16 code, int value)
{
  for(struct evdev_client *c = dev->clients; c; c = c->next)
    evdev_pass_event(c, type, code, value);
  for(int i = 0; i < INPUT_MAX_HANDLERS; i++)
    if(dev->handlers[i])
      dev->handlers[i]->event(dev, type, code, value);
  /* The hook only appends to win32k's raw queue and schedules work; it does
     not compose or sleep while the input spinlock is held.  SYN_REPORT is
     passed on too: win32k applies a mouse frame's motion once per frame. */
  if(win32k_input_event)
    win32k_input_event(type, code, value);
  dev->sync_pending = !(type == EV_SYN && code == SYN_REPORT);
}

// Caller holds input_devices_lock and dev->lock.
static void
input_connect_locked(struct input_dev *dev, struct input_handler *h)
{
  int free = -1;

  for(int i = 0; i < INPUT_MAX_HANDLERS; i++){
    if(dev->handlers[i] == h)
      return;
    if(dev->handlers[i] == 0 && free < 0)
      free = i;
  }
  if(free >= 0){
    dev->handlers[free] = h;
    IN_LOG("connect device=%s -> handler=%s\n", dev->name, h->name);
  }
}

// Already-open files stay attached to their device and see EOF/-1 once they
// have drained its buffered events after it is unplugged.
static int
evdev_open(struct file *f)
{
  struct evdev_client *client;
  struct input_dev *dev;
  int minor = f->ip ? f->ip->minor : 0;

  if(minor < 0 || minor >= INPUT_MAX_DEVICES)
    return -1;
  client = kalloc();
  if(client == 0)
    return -1;
  memset(client, 0, sizeof(*client));

  acquire(&input_devices_lock);
  dev = devices[minor];
  if(dev == 0){
    release(&input_devices_lock);
    kfree(client);
    return -1;              // nothing registered as eventN (Linux: -ENODEV)
  }
  acquire(&dev->lock);
  dev->refs++;
  client->dev = dev;
  client->next = dev->clients;
  dev->clients = client;
  release(&dev->lock);
  release(&input_devices_lock);

  f->private_data = client;
  return 0;
}

static void
evdev_release(struct file *f)
{
  struct evdev_client *client = f->private_data;
  struct input_dev *dev;

  if(client == 0)
    return;
  dev = client->dev;
  acquire(&dev->lock);
  for(struct evdev_client **pp = &dev->clients; *pp; pp = &(*pp)->next)
    if(*pp == client){
      *pp = client->next;
      break;
    }
  release(&dev->lock);
  kfree(client);
  input_put_device(dev);
}

static int
evdev_read(struct file *f, int user_dst, uint64 dst, int n)
{
  struct evdev_client *client = f->private_data;
  struct input_dev *dev;
  struct input_event ev;
  int copied = 0;

  if(client == 0 || n < (int)sizeof(ev))
    return -1;
  dev = client->dev;
  acquire(&dev->lock);
  while(client->packet_head == client->tail){
    // Buffered events are delivered even after unplug; then report the loss.
    if(dev->disconnected || (f->flags & O_NONBLOCK) || myproc()->killed){
      release(&dev->lock);
      return -1;
    }
    sleep(client, &dev->lock);
  }
  while(n - copied >= (int)sizeof(ev) && client->packet_head != client->tail){
    ev = client->buffer[client->tail++ % EVDEV_BUFFER];
    release(&dev->lock);
    if(either_copyout(user_dst, dst + copied, &ev, sizeof(ev)) < 0)
      return copied ? copied : -1;
    copied += sizeof(ev);
    acquire(&dev->lock);
  }
  release(&dev->lock);
  return copied;
}

void
input_init(void)
{
  static struct file_operations evdev_fops = {
    .open = evdev_open,
    .release = evdev_release,
    .fread = evdev_read,
  };

  initlock(&input_devices_lock, "input-devices");
  memset(devices, 0, sizeof(devices));
  memset(handlers, 0, sizeof(handlers));
  if(register_chrdev(INPUT, "input/event", &evdev_fops) < 0)
    panic("input chrdev");
}

struct input_dev *
input_allocate_device(void)
{
  struct input_dev *dev = kalloc();
  if(dev == 0)
    return 0;
  memset(dev, 0, sizeof(*dev));
  initlock(&dev->lock, "input-device");
  dev->refs = 1;            // the driver's reference
  dev->event_number = -1;
  return dev;
}

// Drop the driver's reference.  Open files may keep the memory alive.
void
input_free_device(struct input_dev *dev)
{
  if(dev == 0)
    return;
  if(dev->registered)
    panic("free registered input");
  input_put_device(dev);
}

void
input_set_capability(struct input_dev *dev, int type, int code)
{
  if(dev == 0)
    return;
  if(type == EV_REL && code >= 0 && code <= REL_MAX)
    dev->relbit |= 1U << code;
  else if(type == EV_KEY && code > KEY_RESERVED && code <= KEY_MAX)
    dev->keybit[code / 32] |= 1U << (code % 32);
}

int
input_register_device(struct input_dev *dev)
{
  int slot = -1;

  if(dev == 0 || dev->name == 0)
    return -1;
  acquire(&input_devices_lock);
  if(dev->registered || dev->disconnected){
    release(&input_devices_lock);
    return -1;
  }
  for(int i = 0; i < INPUT_MAX_DEVICES; i++)
    if(devices[i] == 0){
      slot = i;
      break;
    }
  if(slot < 0){
    release(&input_devices_lock);
    printf("input: no slot for %s\n", dev->name);
    return -1;
  }
  devices[slot] = dev;
  acquire(&dev->lock);
  dev->registered = 1;
  dev->event_number = slot;
  for(int i = 0; i < INPUT_MAX_HANDLERS; i++)
    if(handlers[i] && (handlers[i]->match == 0 || handlers[i]->match(dev)))
      input_connect_locked(dev, handlers[i]);
  release(&dev->lock);
  release(&input_devices_lock);
  printf("input: %s registered as /dev/input/event%d\n", dev->name, slot);
  return 0;
}

// Register an in-kernel consumer and connect it to the matching devices
// that are already registered; devices that appear later are connected by
// input_register_device().
int
input_register_handler(struct input_handler *h)
{
  int slot = -1;

  if(h == 0 || h->event == 0)
    return -1;
  acquire(&input_devices_lock);
  for(int i = 0; i < INPUT_MAX_HANDLERS; i++){
    if(handlers[i] == h){
      release(&input_devices_lock);
      return -1;
    }
    if(handlers[i] == 0 && slot < 0)
      slot = i;
  }
  if(slot < 0){
    release(&input_devices_lock);
    return -1;
  }
  handlers[slot] = h;
  for(int i = 0; i < INPUT_MAX_DEVICES; i++){
    struct input_dev *dev = devices[i];
    if(dev == 0 || (h->match && !h->match(dev)))
      continue;
    acquire(&dev->lock);
    input_connect_locked(dev, h);
    release(&dev->lock);
  }
  release(&input_devices_lock);
  printf("input: handler %s registered\n", h->name);
  return 0;
}

// For handlers' match(): does the device report this key?
int
input_has_key(struct input_dev *dev, int code)
{
  if(dev == 0 || code <= KEY_RESERVED || code > KEY_MAX)
    return 0;
  return (dev->keybit[code / 32] & (1U << (code % 32))) != 0;
}

// Never sleeps: open files keep their own reference, so the driver's remove
// path does not wait for user space to close /dev/input/eventN.
void
input_unregister_device(struct input_dev *dev)
{
  if(dev == 0)
    return;
  acquire(&input_devices_lock);
  for(int i = 0; i < INPUT_MAX_DEVICES; i++)
    if(devices[i] == dev)
      devices[i] = 0;
  acquire(&dev->lock);
  // Like Linux input_dev_release_keys(): keys still down at unplug are
  // released, so readers and handlers (shift state!) do not keep them stuck.
  for(int code = 1; code <= KEY_MAX; code++)
    if(dev->keystate[code / 32] & (1U << (code % 32))){
      dev->keystate[code / 32] &= ~(1U << (code % 32));
      input_queue_locked(dev, EV_KEY, code, 0);
    }
  if(dev->sync_pending)
    input_queue_locked(dev, EV_SYN, SYN_REPORT, 0);
  dev->registered = 0;
  dev->disconnected = 1;
  dev->event_number = -1;
  memset(dev->handlers, 0, sizeof(dev->handlers));
  for(struct evdev_client *c = dev->clients; c; c = c->next)
    wakeup(c);
  release(&dev->lock);
  release(&input_devices_lock);
}

void
input_report_key(struct input_dev *dev, int code, int value)
{
  uint32 mask;
  int old;

  if(dev == 0 || code <= KEY_RESERVED || code > KEY_MAX)
    return;
  mask = 1U << (code % 32);
  acquire(&dev->lock);
  if(!dev->registered || dev->disconnected ||
     !(dev->keybit[code / 32] & mask)){
    release(&dev->lock);
    return;
  }
  old = (dev->keystate[code / 32] & mask) != 0;
  value = value != 0;
  if(old == value){
    release(&dev->lock);
    return;
  }
  if(value)
    dev->keystate[code / 32] |= mask;
  else
    dev->keystate[code / 32] &= ~mask;
  input_queue_locked(dev, EV_KEY, code, value);
  release(&dev->lock);
}

// Relative motion (mice): no state, so every non-zero delta is an event.
void
input_report_rel(struct input_dev *dev, int code, int value)
{
  if(dev == 0 || code < 0 || code > REL_MAX || value == 0)
    return;
  acquire(&dev->lock);
  if(dev->registered && !dev->disconnected && (dev->relbit & (1U << code)))
    input_queue_locked(dev, EV_REL, code, value);
  release(&dev->lock);
}

void
input_sync(struct input_dev *dev)
{
  if(dev == 0)
    return;
  acquire(&dev->lock);
  // Like Linux, a frame with no change produces no SYN_REPORT.
  if(dev->registered && !dev->disconnected && dev->sync_pending)
    input_queue_locked(dev, EV_SYN, SYN_REPORT, 0);
  release(&dev->lock);
}

subsys_initcall("input", input_init);
