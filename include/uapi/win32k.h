#ifndef _UAPI_WIN32K_H
#define _UAPI_WIN32K_H

#include "types.h"

/* The xv6 "win32k" system call: one entry point multiplexing the window
 * manager services, like the NtUser* service table of Windows NT.
 *
 *   long win32k(int op, long a1, long a2, long a3, long a4);
 *
 * Top-level windows, their Z order, per-process message queues, input
 * routing, and the visible-region bookkeeping live in the kernel.  Child
 * controls and drawing stay in the user32 library of each process. */

#define W32_CREATE      1   /* (struct w32_create *) -> hwnd */
#define W32_DESTROY     2   /* (hwnd) */
#define W32_SHOW        3   /* (hwnd, show): 0 minimize, 1 restore+activate */
#define W32_MOVE        4   /* (hwnd, struct w32_rect *) */
#define W32_SETTEXT     5   /* (hwnd, const char *) */
#define W32_GETTEXT     6   /* (hwnd, char *, max) */
#define W32_GETINFO     7   /* (hwnd, struct w32_info *) */
#define W32_INVALIDATE  8   /* (hwnd, struct w32_rect * window coords | 0) */
#define W32_GETMSG      9   /* (struct w32_msg *, timeout_ms | -1) -> 1/0 */
#define W32_POST        10  /* (struct w32_msg *) to the hwnd's owner */
#define W32_BEGINPAINT  11  /* (hwnd, struct w32_paint *) */
#define W32_ENDPAINT    12  /* (hwnd) */
#define W32_ACTIVATE    13  /* (hwnd) raise and activate */
#define W32_GETACTIVE   14  /* () -> hwnd */
#define W32_ENUM        15  /* (uint32 *out, max, all) -> n; creation order */
#define W32_SETSHELL    16  /* (hwnd) receives WM_SHELLHOOK on changes */
#define W32_QUIT        17  /* (code) post WM_QUIT to the caller's queue */

/* Window styles known to the kernel. */
#define W32S_VISIBLE    0x01
#define W32S_CAPTION    0x02   /* 3D frame + title bar + min/close buttons */
#define W32S_TOPMOST    0x04
#define W32S_BOTTOM     0x08   /* the desktop: always at the bottom */
#define W32S_NOTASKBAR  0x10
#define W32S_MENU       0x20   /* popup menu: a click elsewhere cancels it */

/* Frame metrics shared by the kernel (hit testing) and user32 (drawing). */
#define W32_FRAME       3
#define W32_CAPTION_H   18
#define W32_BTN_W       16
#define W32_BTN_H       14
/* Caption buttons, in window coordinates (width = window width):
 *   close    x = width - W32_FRAME - 2 - W32_BTN_W
 *   minimize x = close.x - 18;  both at y = W32_FRAME + 2 */

/* Kernel-generated messages (values as in Win32). */
#define W32M_ACTIVATE    0x0006
#define W32M_PAINT       0x000F
#define W32M_CLOSE       0x0010
#define W32M_QUIT        0x0012
#define W32M_CANCELMODE  0x001F
#define W32M_KEYDOWN     0x0100
#define W32M_CHAR        0x0102
#define W32M_MOUSEMOVE   0x0200
#define W32M_LBUTTONDOWN 0x0201
#define W32M_LBUTTONUP   0x0202
#define W32M_LBUTTONDBLCLK 0x0203
#define W32M_RBUTTONDOWN 0x0204
#define W32M_MOUSEWHEEL  0x020A
#define W32M_SHELLHOOK   0x0501   /* WM_USER + 0x101 */

struct w32_rect {
  int left, top, right, bottom;
};

/* Mouse messages carry window coordinates (frame included) in lParam:
 * x in the low 16 bits, y in the high 16 bits. */
struct w32_msg {
  uint32 hwnd;
  uint32 message;
  uint64 wparam;
  long lparam;
};

struct w32_create {
  int x, y, w, h;               /* screen rectangle of the whole window */
  uint32 style;
  char title[64];
};

struct w32_info {
  struct w32_rect rc;           /* screen */
  uint32 style;
  int owner;                    /* process id */
  int minimized;
  int active;
};

#define W32_MAXVIS 32
struct w32_paint {
  struct w32_rect rc;           /* the window on screen */
  struct w32_rect update;       /* what needs repainting, window coords */
  int active;                   /* draw the caption as active */
  int nvis;                     /* visible (unobscured) parts of update */
  struct w32_rect vis[W32_MAXVIS];   /* screen coords */
};

#endif
