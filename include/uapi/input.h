#ifndef _UAPI_INPUT_H
#define _UAPI_INPUT_H

#include "types.h"

/* User-space view of /dev/input/eventN: a subset of Linux
 * include/uapi/linux/input.h and input-event-codes.h with the same values.
 * (xv6's struct input_event has no timestamp.) */
struct input_event {
  uint16 type;
  uint16 code;
  int value;
};

#define EVIOCGRAB     0x40044590   /* ioctl(fd, EVIOCGRAB, 1 | 0) */

#define EV_SYN        0
#define EV_KEY        1
#define EV_REL        2

#define SYN_REPORT    0
#define SYN_DROPPED   3

#define REL_X         0x00
#define REL_Y         0x01
#define REL_WHEEL     0x08

#define BTN_LEFT      0x110
#define BTN_RIGHT     0x111
#define BTN_MIDDLE    0x112

#define KEY_ESC         1
#define KEY_BACKSPACE  14
#define KEY_TAB        15
#define KEY_ENTER      28
#define KEY_LEFTCTRL   29
#define KEY_LEFTSHIFT  42
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

#endif
