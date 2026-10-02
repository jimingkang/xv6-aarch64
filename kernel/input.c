// Minimal Linux-style input core and evdev bridge.
//
// Device drivers report state changes through input_report_key()/input_sync().
// The input core keeps the current key state and fans events out to every
// open file of /dev/input/event0 (character major INPUT).  Like Linux evdev,
// each open file gets its own buffer, so readers do not steal events from one
// another and never see events from before they opened the device.

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

// Both live in a single kalloc() page.
_Static_assert(sizeof(struct input_dev) <= PGSIZE, "input_dev too large");
_Static_assert(sizeof(struct evdev_client) <= PGSIZE, "evdev_client too large");
_Static_assert((EVDEV_BUFFER & (EVDEV_BUFFER - 1)) == 0,
               "EVDEV_BUFFER must be a power of two");

// Lock order: input_devices_lock, then input_dev.lock.
static struct spinlock input_devices_lock;
static struct input_dev *devices[INPUT_MAX_DEVICES];
static struct input_dev *event0;   // device currently behind /dev/input/event0

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
  dev->sync_pending = !(type == EV_SYN && code == SYN_REPORT);
}

// Caller holds input_devices_lock.  Hand event0 to another registered device.
// Already-open files stay attached to the old device and see EOF/-1 once they
// have drained its buffered events; new opens get the promoted device.
static void
input_promote_locked(void)
{
  event0 = 0;
  for(int i = 0; i < INPUT_MAX_DEVICES; i++){
    struct input_dev *dev = devices[i];
    if(dev == 0)
      continue;
    acquire(&dev->lock);
    dev->event_number = 0;
    release(&dev->lock);
    event0 = dev;
    printf("input: %s is now event0\n", dev->name);
    return;
  }
}

static int
evdev_open(struct file *f)
{
  struct evdev_client *client;
  struct input_dev *dev;

  client = kalloc();
  if(client == 0)
    return -1;
  memset(client, 0, sizeof(*client));

  acquire(&input_devices_lock);
  dev = event0;
  if(dev == 0){
    release(&input_devices_lock);
    kfree(client);
    return -1;              // no keyboard attached (Linux: -ENODEV)
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
  event0 = 0;
  if(register_chrdev(INPUT, "input/event0", &evdev_fops) < 0)
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
  if(dev == 0 || type != EV_KEY || code <= KEY_RESERVED || code > KEY_MAX)
    return;
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
  dev->registered = 1;
  if(event0 == 0){
    event0 = dev;
    dev->event_number = 0;
  }
  release(&input_devices_lock);
  printf("input: %s registered as event%d\n", dev->name,
         dev->event_number);
  return 0;
}

// Never sleeps: open files keep their own reference, so the driver's remove
// path does not wait for user space to close /dev/input/event0.
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
  dev->registered = 0;
  dev->disconnected = 1;
  dev->event_number = -1;
  for(struct evdev_client *c = dev->clients; c; c = c->next)
    wakeup(c);
  release(&dev->lock);
  if(event0 == dev)
    input_promote_locked();
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

void
input_sync(struct input_dev *dev)
{
  if(dev == 0)
    return;
  acquire(&dev->lock);
  // Like Linux, a frame with no state change produces no SYN_REPORT.
  if(dev->registered && !dev->disconnected && dev->sync_pending)
    input_queue_locked(dev, EV_SYN, SYN_REPORT, 0);
  release(&dev->lock);
}
