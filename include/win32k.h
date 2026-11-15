#ifndef XV6_WIN32K_UAPI_H
#define XV6_WIN32K_UAPI_H

#include "kernel/types.h"

/* One multiplexed service, mirroring the NtUser/NtGdi boundary. */
#define W32_CREATE       1
#define W32_DESTROY      2
#define W32_SHOW         3
#define W32_MOVE         4
#define W32_SETTEXT      5
#define W32_GETTEXT      6
#define W32_GETINFO      7
#define W32_INVALIDATE   8
#define W32_GETMSG       9
#define W32_POST        10
#define W32_BEGINPAINT  11
#define W32_ENDPAINT    12
#define W32_ACTIVATE    13
#define W32_GETACTIVE   14
#define W32_QUIT        17
#define W32_GDI         18
#define W32_SCREEN      19

#define GDI_FILL 1
#define GDI_TEXT 2

#define W32S_VISIBLE 0x01
#define W32S_CAPTION 0x02

#define W32M_ACTIVATE 0x0006
#define W32M_PAINT    0x000f
#define W32M_CLOSE    0x0010
#define W32M_QUIT     0x0012
#define W32M_KEYDOWN  0x0100
#define W32M_CHAR     0x0102

struct w32_rect { int left, top, right, bottom; };
struct w32_msg { uint32 hwnd, message; uint64 wparam; long lparam; };
struct w32_create { int x, y, w, h; uint32 style; char title[64]; };
struct w32_info {
  struct w32_rect rc;
  uint32 style;
  int owner, minimized, active;
};
struct w32_paint {
  struct w32_rect rc, update;
  int active;
};
struct w32_gdicmd {
  uint16 op, len;
  int x0, y0, x1, y1;
  uint32 color;
  int clip_left, clip_top, clip_right, clip_bottom;
};

#endif
