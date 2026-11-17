#ifndef XV6_INPUT_H
#define XV6_INPUT_H

#include "spinlock.h"
#include "linux/input.h"

#define INPUT_KEY_WORDS ((KEY_MAX + 32) / 32)
#define INPUT_MAX_DEVICES 4   // /dev/input/event0..3 (minor = event number)
#define INPUT_MAX_HANDLERS 4
#define EVDEV_BUFFER 256    // events per open file; power of two

struct input_dev;

// An in-kernel consumer of input events, like Linux struct input_handler
// (keyboard.c's kbd_handler, mousedev, ...).  evdev is built into the core;
// other handlers register here and are connected to every device whose
// capabilities match(), whether the device appears before or after them.
// event() runs with the device's spinlock held (possibly in interrupt
// context): it must not sleep and must not call back into the input core.
struct input_handler {
  char *name;
  int (*match)(struct input_dev *dev);
  void (*event)(struct input_dev *dev, uint16 type, uint16 code, int value);
};

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
  uint32 relbit;                  // EV_REL axes the device reports
  struct evdev_client *clients;
  struct input_handler *handlers[INPUT_MAX_HANDLERS];   // connected handlers
  int refs;
  int registered;
  int disconnected;
  int event_number;               // N of /dev/input/eventN, -1 when gone
  int sync_pending;               // events queued since the last SYN_REPORT
};

void input_init(void);
struct input_dev *input_allocate_device(void);
void input_free_device(struct input_dev *dev);
void input_set_capability(struct input_dev *dev, int type, int code);
int input_register_device(struct input_dev *dev);
void input_unregister_device(struct input_dev *dev);
void input_report_key(struct input_dev *dev, int code, int value);
void input_report_rel(struct input_dev *dev, int code, int value);
void input_sync(struct input_dev *dev);
int input_register_handler(struct input_handler *handler);
int input_has_key(struct input_dev *dev, int code);

#endif
