#ifndef XV6_INPUT_H
#define XV6_INPUT_H

#include "spinlock.h"

#define EV_SYN 0
#define EV_KEY 1
#define EV_REL 2

#define SYN_REPORT  0
#define SYN_DROPPED 3

// Linux input-event key codes used by the HID boot-keyboard driver.
#define KEY_RESERVED    0
#define KEY_ESC         1
#define KEY_1           2
#define KEY_2           3
#define KEY_3           4
#define KEY_4           5
#define KEY_5           6
#define KEY_6           7
#define KEY_7           8
#define KEY_8           9
#define KEY_9          10
#define KEY_0          11
#define KEY_MINUS      12
#define KEY_EQUAL      13
#define KEY_BACKSPACE  14
#define KEY_TAB        15
#define KEY_Q          16
#define KEY_W          17
#define KEY_E          18
#define KEY_R          19
#define KEY_T          20
#define KEY_Y          21
#define KEY_U          22
#define KEY_I          23
#define KEY_O          24
#define KEY_P          25
#define KEY_LEFTBRACE  26
#define KEY_RIGHTBRACE 27
#define KEY_ENTER      28
#define KEY_LEFTCTRL   29
#define KEY_A          30
#define KEY_S          31
#define KEY_D          32
#define KEY_F          33
#define KEY_G          34
#define KEY_H          35
#define KEY_J          36
#define KEY_K          37
#define KEY_L          38
#define KEY_SEMICOLON  39
#define KEY_APOSTROPHE 40
#define KEY_GRAVE      41
#define KEY_LEFTSHIFT  42
#define KEY_BACKSLASH  43
#define KEY_Z          44
#define KEY_X          45
#define KEY_C          46
#define KEY_V          47
#define KEY_B          48
#define KEY_N          49
#define KEY_M          50
#define KEY_COMMA      51
#define KEY_DOT        52
#define KEY_SLASH      53
#define KEY_RIGHTSHIFT 54
#define KEY_LEFTALT    56
#define KEY_SPACE      57
#define KEY_CAPSLOCK   58
#define KEY_RIGHTCTRL  97
#define KEY_RIGHTALT  100
#define KEY_UP        103
#define KEY_LEFT      105
#define KEY_RIGHT     106
#define KEY_DOWN      108
#define KEY_LEFTMETA  125
#define KEY_RIGHTMETA 126
#define BTN_LEFT      0x110
#define BTN_RIGHT     0x111
#define BTN_MIDDLE    0x112
#define KEY_MAX       0x2ff

#define REL_X         0x00
#define REL_Y         0x01
#define REL_WHEEL     0x08
#define REL_MAX       0x0f

#define INPUT_KEY_WORDS ((KEY_MAX + 32) / 32)
#define INPUT_MAX_DEVICES 4
#define EVDEV_BUFFER 256    // events per open file; power of two

struct input_event {
  uint16 type;
  uint16 code;
  int value;
};

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
  uint32 relbit;
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
void input_report_rel(struct input_dev *dev, int code, int value);
void input_sync(struct input_dev *dev);

#endif
