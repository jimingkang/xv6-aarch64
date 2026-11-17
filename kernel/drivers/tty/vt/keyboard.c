/* Console keyboard handler (Linux drivers/tty/vt/keyboard.c, much smaller).
 *
 * An input_handler that turns EV_KEY events from any real keyboard into
 * characters for the console line discipline, the same consoleintr() path
 * the serial port uses, so echo, erase, ^C and wakeups stay in one place:
 *
 *   usbkbd (USB HID) --+
 *                      +--> input core --> kbd handler --> consoleintr()
 *   other keyboards ---+               \--> /dev/input/eventN (evdev)
 *
 * It connects to devices that report letter keys, so the Tiny210 K1..K8
 * buttons (KEY_1..KEY_8 only) stay evdev-only.  Shift, Ctrl and Caps Lock
 * state is global, as with a Linux VT: Shift on one keyboard and a letter on
 * another make a capital.  While the win32k window manager owns keyboard
 * input, characters go to its windows instead of the console.
 */
#include "kernel/types.h"
#include "kernel/aarch64.h"
#include "kernel/spinlock.h"
#include "kernel/defs.h"
#include "kernel/input.h"
#include "kernel/module.h"

int win32k_owns_input(void) __attribute__((weak));

#ifndef USBHID_TRACE
#define USBHID_TRACE 0
#endif

#define MOD_LSHIFT (1U << 0)
#define MOD_RSHIFT (1U << 1)
#define MOD_LCTRL  (1U << 2)
#define MOD_RCTRL  (1U << 3)
#define MOD_SHIFT  (MOD_LSHIFT | MOD_RSHIFT)
#define MOD_CTRL   (MOD_LCTRL | MOD_RCTRL)

static struct spinlock kbd_lock;   // taken inside input_dev.lock
static uint modifiers;
static int capslock;

/* US layout, KEY_* code -> character, without and with Shift. */
static const char plain_map[KEY_SPACE + 1] = {
  [KEY_ESC] = 0x1b,
  [KEY_1] = '1', [KEY_2] = '2', [KEY_3] = '3', [KEY_4] = '4', [KEY_5] = '5',
  [KEY_6] = '6', [KEY_7] = '7', [KEY_8] = '8', [KEY_9] = '9', [KEY_0] = '0',
  [KEY_MINUS] = '-', [KEY_EQUAL] = '=', [KEY_BACKSPACE] = '\b',
  [KEY_TAB] = '\t',
  [KEY_Q] = 'q', [KEY_W] = 'w', [KEY_E] = 'e', [KEY_R] = 'r', [KEY_T] = 't',
  [KEY_Y] = 'y', [KEY_U] = 'u', [KEY_I] = 'i', [KEY_O] = 'o', [KEY_P] = 'p',
  [KEY_LEFTBRACE] = '[', [KEY_RIGHTBRACE] = ']', [KEY_ENTER] = '\n',
  [KEY_A] = 'a', [KEY_S] = 's', [KEY_D] = 'd', [KEY_F] = 'f', [KEY_G] = 'g',
  [KEY_H] = 'h', [KEY_J] = 'j', [KEY_K] = 'k', [KEY_L] = 'l',
  [KEY_SEMICOLON] = ';', [KEY_APOSTROPHE] = '\'', [KEY_GRAVE] = '`',
  [KEY_BACKSLASH] = '\\',
  [KEY_Z] = 'z', [KEY_X] = 'x', [KEY_C] = 'c', [KEY_V] = 'v', [KEY_B] = 'b',
  [KEY_N] = 'n', [KEY_M] = 'm',
  [KEY_COMMA] = ',', [KEY_DOT] = '.', [KEY_SLASH] = '/', [KEY_SPACE] = ' ',
};

static const char shift_map[KEY_SPACE + 1] = {
  [KEY_ESC] = 0x1b,
  [KEY_1] = '!', [KEY_2] = '@', [KEY_3] = '#', [KEY_4] = '$', [KEY_5] = '%',
  [KEY_6] = '^', [KEY_7] = '&', [KEY_8] = '*', [KEY_9] = '(', [KEY_0] = ')',
  [KEY_MINUS] = '_', [KEY_EQUAL] = '+', [KEY_BACKSPACE] = '\b',
  [KEY_TAB] = '\t',
  [KEY_Q] = 'Q', [KEY_W] = 'W', [KEY_E] = 'E', [KEY_R] = 'R', [KEY_T] = 'T',
  [KEY_Y] = 'Y', [KEY_U] = 'U', [KEY_I] = 'I', [KEY_O] = 'O', [KEY_P] = 'P',
  [KEY_LEFTBRACE] = '{', [KEY_RIGHTBRACE] = '}', [KEY_ENTER] = '\n',
  [KEY_A] = 'A', [KEY_S] = 'S', [KEY_D] = 'D', [KEY_F] = 'F', [KEY_G] = 'G',
  [KEY_H] = 'H', [KEY_J] = 'J', [KEY_K] = 'K', [KEY_L] = 'L',
  [KEY_SEMICOLON] = ':', [KEY_APOSTROPHE] = '"', [KEY_GRAVE] = '~',
  [KEY_BACKSLASH] = '|',
  [KEY_Z] = 'Z', [KEY_X] = 'X', [KEY_C] = 'C', [KEY_V] = 'V', [KEY_B] = 'B',
  [KEY_N] = 'N', [KEY_M] = 'M',
  [KEY_COMMA] = '<', [KEY_DOT] = '>', [KEY_SLASH] = '?', [KEY_SPACE] = ' ',
};

static uint
modifier_bit(int code)
{
  switch(code){
  case KEY_LEFTSHIFT:  return MOD_LSHIFT;
  case KEY_RIGHTSHIFT: return MOD_RSHIFT;
  case KEY_LEFTCTRL:   return MOD_LCTRL;
  case KEY_RIGHTCTRL:  return MOD_RCTRL;
  }
  return 0;
}

static int
is_letter(int code)
{
  char c = code <= KEY_SPACE ? plain_map[code] : 0;
  return c >= 'a' && c <= 'z';
}

/* Arrow keys become the ANSI cursor sequences a terminal would send. */
static char
arrow_final(int code)
{
  switch(code){
  case KEY_UP:    return 'A';
  case KEY_DOWN:  return 'B';
  case KEY_RIGHT: return 'C';
  case KEY_LEFT:  return 'D';
  }
  return 0;
}

static void
kbd_event(struct input_dev *dev, uint16 type, uint16 code, int value)
{
  char out[3];
  int n = 0;

  if(type != EV_KEY)
    return;
  acquire(&kbd_lock);
  uint bit = modifier_bit(code);
  if(bit){
    if(value)
      modifiers |= bit;
    else
      modifiers &= ~bit;
  } else if(value && code == KEY_CAPSLOCK){
    capslock ^= 1;
  } else if(value){
    char f = arrow_final(code);
    if(f){
      out[n++] = 0x1b;
      out[n++] = '[';
      out[n++] = f;
    } else if(code <= KEY_SPACE && plain_map[code]){
      int shifted = (modifiers & MOD_SHIFT) != 0;
      if(is_letter(code))
        shifted ^= capslock;
      char c = shifted ? shift_map[code] : plain_map[code];
      if((modifiers & MOD_CTRL) && c >= 'a' && c <= 'z')
        c = c - 'a' + 1;
      else if((modifiers & MOD_CTRL) && c >= 'A' && c <= 'Z')
        c = c - 'A' + 1;
      out[n++] = c;
    }
  }
  release(&kbd_lock);
  if(n && win32k_owns_input && win32k_owns_input()){
#if USBHID_TRACE >= 2
    printf("kbd: KEY %d -> win32k (console skipped)\n", code);
#endif
    return;
  }
#if USBHID_TRACE >= 2
  if(n)
    printf("kbd: KEY %d -> console %x%s mods=%x caps=%d\n", code,
           (uchar)out[0], n > 1 ? "..." : "", modifiers, capslock);
#endif
  for(int i = 0; i < n; i++)
    consoleintr(out[i]);
}

/* Linux kbd_match: a keyboard has ordinary keys, not just a few buttons. */
static int
kbd_match(struct input_dev *dev)
{
  return input_has_key(dev, KEY_A) && input_has_key(dev, KEY_ENTER);
}

static struct input_handler kbd_handler = {
  .name = "kbd",
  .match = kbd_match,
  .event = kbd_event,
};

static void
kbd_init(void)
{
  initlock(&kbd_lock, "kbd");
  input_register_handler(&kbd_handler);
}

/* After the input core (subsys level), before keyboards probe. */
fs_initcall("kbd", kbd_init);
