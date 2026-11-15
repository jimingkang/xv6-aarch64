#ifndef XV6_INPUT_H
#define XV6_INPUT_H

#include "spinlock.h"
#include "linux/input.h"

#define INPUT_KEY_WORDS ((KEY_MAX + 32) / 32)
#define INPUT_MAX_DEVICES 4
#define EVDEV_BUFFER 256    // events per open file; power of two

struct input_dev;

// One per open("/dev/input/event0"), like Linux struct evdev_client.  Each
// reader sees every event from the moment it opened the device, independently
// of other readers.  Only whole frames are readable: events in
// [packet_head, head) belong to a frame whose SYN_REPORT has not arrived yet.
// Protected by dev->lock.
struct evdev_client {
  struct input_dev *dev;          // holds a reference (input_dev.refs)
  struct evdev_client *next;      // dev->clients list
  uint head;                      // next write slot
  uint packet_head;               // end of the last complete frame
  uint tail;                      // next read slot
  struct input_event buffer[EVDEV_BUFFER];
};

// Ownership: input_allocate_device() returns a device holding one reference
// for the driver.  Each evdev client holds another.  The driver gives up its
// reference with input_free_device() (only after input_unregister_device()),
// and the memory is released when the last open file is closed.
struct input_dev {
  struct spinlock lock;           // protects everything below
  char *name;
  char *phys;
  uint16 bustype;
  uint16 vendor;
  uint16 product;
  uint16 version;
  void *private;
  uint32 keybit[INPUT_KEY_WORDS];
  uint32 keystate[INPUT_KEY_WORDS];
  struct evdev_client *clients;
  int refs;
  int registered;
  int disconnected;
  int event_number;
  int sync_pending;               // EV_KEY queued since the last SYN_REPORT
};

void input_init(void);
struct input_dev *input_allocate_device(void);
void input_free_device(struct input_dev *dev);
void input_set_capability(struct input_dev *dev, int type, int code);
int input_register_device(struct input_dev *dev);
void input_unregister_device(struct input_dev *dev);
void input_report_key(struct input_dev *dev, int code, int value);
void input_sync(struct input_dev *dev);

#endif
