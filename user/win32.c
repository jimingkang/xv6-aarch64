// win32.c: user32 + gdi32 for xv6, on top of the kernel window manager.
//
// Since the win32k step the split follows Windows NT:
//
//   kernel (drivers/video/win32k.c)    top-level windows (HWND handles), Z
//                                      order, activation, per-process message
//                                      queues, input hit testing, caption
//                                      drag/minimize/close, the mouse cursor,
//                                      update regions and visible regions
//   this library, in every process     child controls (BUTTON, EDIT, ...),
//                                      window procedures, DefWindowProc,
//                                      GDI drawing, popup menus, MessageBox
//
// GetMessage() blocks in the kernel.  When the kernel reports WM_PAINT for
// one of our top-level windows, we draw its frame, client area and children
// into a private screen-sized back buffer, then copy only the visible
// rectangles the kernel returned by BeginPaint to /dev/fb0.  So several
// processes draw on one screen without overwriting each other, as GDI did
// before the desktop window manager composited windows.

#include <uapi/types.h>
#include <uapi/stat.h>
#include "user/user.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <uapi/fb.h>
#include <uapi/win32k.h>
#include "user/win32.h"
#include "user/font8x16.h"

#define MAXWIN     128
#define MAXCLASS   32
#define MAXQUEUE   256
#define MAXTIMER   32

#define FRAME      W32_FRAME
#define CAPTION_H  W32_CAPTION_H
#define CBORDER    2            // WS_BORDER sunken control border
// WS_CAPTION is WS_BORDER | WS_DLGFRAME (as in Win32), so a bordered control
// also has one of its bits: a caption needs both.
#define HASCAP(style) (((style) & WS_CAPTION) == WS_CAPTION)
#define WS_MENU_INTERNAL 0x00000020u

struct wclass {
  int used;
  char name[24];
  WNDPROC proc;
  COLORREF bg;
};

struct window {
  int used;
  struct wclass *cls;
  WNDPROC proc;
  char *text;
  uint style;
  RECT rc;                      // top-level: screen; child: parent client
  struct window *parent;
  struct window *child, *next;  // children in paint order
  int id;
  long userdata;
  int visible, minimized;
  void *ctl;                    // built-in control state
  uint32 khwnd;                 // top-level: the kernel's handle
  struct window *focus;         // top-level: focused descendant
};

struct dc {
  HWND hwnd;
  int ox, oy;                   // client origin on screen
  RECT clip;                    // screen coordinates
  COLORREF text, bk;
  int opaque;
};

static struct wclass classes[MAXCLASS];
static struct window windows[MAXWIN];

static MSG queue[MAXQUEUE];
static int qhead, qtail;
static int quit_posted;

static struct { HWND hwnd; int id, ms; uint64 next; } timers[MAXTIMER];

static HWND active;             // the top-level being painted, if active
static HWND active_top;         // our top-level the kernel last activated
static HWND capture;            // control that got the button down
static HWND popup;              // our open popup menu
static HWND nc_button_win;      // (the kernel tracks caption buttons)
static int nc_button;

static long
k(int op, long a1, long a2, long a3)
{
  return win32k(op, a1, a2, a3);
}

// strncpy that always terminates (the kernel's safestrcpy).
static void
scopy(char *d, const char *s, int n)
{
  if(n <= 0)
    return;
  while(--n > 0 && *s)
    *d++ = *s++;
  *d = 0;
}

// ---------------------------------------------------------------------------
// Screen.

static uint32 *fb, *bb;         // frame buffer; this process's back buffer
static int SW, SH, stride;
static int roff, goff, boff;

static uint32
px(COLORREF c)
{
  return (GetRValue(c) << roff) | (GetGValue(c) << goff) |
         (GetBValue(c) << boff);
}

static int
rect_empty(const RECT *r)
{
  return r->right <= r->left || r->bottom <= r->top;
}

static void
rect_intersect(RECT *d, const RECT *a, const RECT *b)
{
  d->left = a->left > b->left ? a->left : b->left;
  d->top = a->top > b->top ? a->top : b->top;
  d->right = a->right < b->right ? a->right : b->right;
  d->bottom = a->bottom < b->bottom ? a->bottom : b->bottom;
}

static int
pt_in(const RECT *r, int x, int y)
{
  return x >= r->left && x < r->right && y >= r->top && y < r->bottom;
}

static void
sfill(const RECT *clip, int x0, int y0, int x1, int y1, COLORREF c)
{
  RECT r = { x0, y0, x1, y1 }, d;
  rect_intersect(&d, &r, clip);
  if(rect_empty(&d))
    return;
  uint32 p = px(c);
  for(int y = d.top; y < d.bottom; y++){
    uint32 *row = bb + y * SW;
    for(int x = d.left; x < d.right; x++)
      row[x] = p;
  }
}

static void
sglyph(const RECT *clip, int x, int y, int ch, COLORREF fg, int bold)
{
  if(ch < FONT_FIRST || ch > FONT_LAST)
    return;
  const uchar *g = font8x16[ch - FONT_FIRST];
  uint32 p = px(fg);
  for(int r = 0; r < FONT_H; r++){
    int yy = y + r;
    if(yy < clip->top || yy >= clip->bottom)
      continue;
    uint bits = g[r];
    if(bold)
      bits |= bits >> 1;
    for(int c = 0; c < FONT_W; c++){
      int xx = x + c;
      if(xx >= clip->left && xx < clip->right && (bits & (0x80 >> c)))
        bb[yy * SW + xx] = p;
    }
  }
}

static void
stext(const RECT *clip, int x, int y, const char *s, int n, COLORREF fg,
      int bold)
{
  for(int i = 0; i < n && s[i]; i++, x += FONT_W)
    sglyph(clip, x, y, s[i], fg, bold);
}

static void
sedge(const RECT *clip, RECT r, int edge)
{
  COLORREF tl1, br1, tl2, br2;
  if(edge == EDGE_SUNKEN){
    tl1 = CLR_SHADOW; br1 = CLR_HILIGHT; tl2 = CLR_DKSHADOW; br2 = CLR_LIGHT;
  } else if(edge == EDGE_ETCHED){
    tl1 = CLR_SHADOW; br1 = CLR_HILIGHT; tl2 = CLR_HILIGHT; br2 = CLR_SHADOW;
  } else {
    tl1 = CLR_LIGHT; br1 = CLR_DKSHADOW; tl2 = CLR_HILIGHT; br2 = CLR_SHADOW;
  }
  sfill(clip, r.left, r.top, r.right, r.top + 1, tl1);
  sfill(clip, r.left, r.top, r.left + 1, r.bottom, tl1);
  sfill(clip, r.left, r.bottom - 1, r.right, r.bottom, br1);
  sfill(clip, r.right - 1, r.top, r.right, r.bottom, br1);
  r.left++; r.top++; r.right--; r.bottom--;
  sfill(clip, r.left, r.top, r.right, r.top + 1, tl2);
  sfill(clip, r.left, r.top, r.left + 1, r.bottom, tl2);
  sfill(clip, r.left, r.bottom - 1, r.right, r.bottom, br2);
  sfill(clip, r.right - 1, r.top, r.right, r.bottom, br2);
}

// ---------------------------------------------------------------------------
// Window geometry.

static int
valid(HWND w)
{
  return w && w >= windows && w < windows + MAXWIN && w->used;
}

int IsWindow(HWND h) { return valid(h); }

static void client_origin(HWND w, int *x, int *y);

static void
window_screen_rect(HWND w, RECT *r)
{
  if(!w->parent){
    *r = w->rc;
    return;
  }
  int ox, oy;
  client_origin(w->parent, &ox, &oy);   // children are placed in it
  r->left = ox + w->rc.left;
  r->top = oy + w->rc.top;
  r->right = ox + w->rc.right;
  r->bottom = oy + w->rc.bottom;
}

static void
client_origin(HWND w, int *x, int *y)
{
  RECT r;
  window_screen_rect(w, &r);
  int inset = HASCAP(w->style) ? FRAME : (w->style & WS_BORDER) ? CBORDER : 0;
  *x = r.left + inset;
  *y = r.top + inset + (HASCAP(w->style) ? CAPTION_H : 0);
}

static void
client_screen_rect(HWND w, RECT *c)
{
  RECT r;
  window_screen_rect(w, &r);
  int inset = HASCAP(w->style) ? FRAME : (w->style & WS_BORDER) ? CBORDER : 0;
  c->left = r.left + inset;
  c->top = r.top + inset + (HASCAP(w->style) ? CAPTION_H : 0);
  c->right = r.right - inset;
  c->bottom = r.bottom - inset;
}

static HWND
toplevel(HWND w)
{
  while(w && w->parent)
    w = w->parent;
  return w;
}

// The kernel may have moved a top-level window (caption drag): refresh.
static void
sync_rect(HWND w)
{
  struct w32_info info;
  if(w && !w->parent && w->khwnd &&
     k(W32_GETINFO, w->khwnd, (long)&info, 0) == 0){
    w->rc.left = info.rc.left;
    w->rc.top = info.rc.top;
    w->rc.right = info.rc.right;
    w->rc.bottom = info.rc.bottom;
    w->minimized = info.minimized;
  }
}

int
GetClientRect(HWND h, RECT *rc)
{
  if(!valid(h))
    return 0;
  RECT c;
  client_screen_rect(h, &c);
  rc->left = rc->top = 0;
  rc->right = c.right - c.left;
  rc->bottom = c.bottom - c.top;
  return 1;
}

int
GetWindowRect(HWND h, RECT *rc)
{
  if(!valid(h))
    return 0;
  sync_rect(toplevel(h));
  window_screen_rect(h, rc);
  return 1;
}

int IsWindowVisible(HWND h) { return valid(h) && h->visible && !h->minimized; }
int IsIconic(HWND h) { return valid(h) && h->minimized; }
HWND GetParent(HWND h) { return valid(h) ? h->parent : 0; }

int
InvalidateRect(HWND h, const RECT *rc, int erase)
{
  (void)erase;
  if(!valid(h))
    return 0;
  HWND t = toplevel(h);
  if(!t->khwnd)
    return 0;
  RECT r;
  if(rc){
    int ox, oy;
    client_origin(h, &ox, &oy);
    r.left = rc->left + ox; r.right = rc->right + ox;
    r.top = rc->top + oy; r.bottom = rc->bottom + oy;
  } else {
    window_screen_rect(h, &r);
  }
  struct w32_rect wr = { r.left - t->rc.left, r.top - t->rc.top,
                         r.right - t->rc.left, r.bottom - t->rc.top };
  k(W32_INVALIDATE, t->khwnd, (long)&wr, 0);
  return 1;
}

static void
damage_window(HWND w)
{
  InvalidateRect(w, 0, 1);
}

int
GetSystemMetrics(int i)
{
  switch(i){
  case SM_CXSCREEN: return SW;
  case SM_CYSCREEN: return SH;
  case SM_CYCAPTION: return CAPTION_H;
  case SM_CYTASKBAR: return 28;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Classes and windows.

static LRESULT ButtonProc(HWND, uint, WPARAM, LPARAM);
static LRESULT StaticProc(HWND, uint, WPARAM, LPARAM);
static LRESULT EditProc(HWND, uint, WPARAM, LPARAM);
static LRESULT ListBoxProc(HWND, uint, WPARAM, LPARAM);
static LRESULT MenuProc(HWND, uint, WPARAM, LPARAM);
static LRESULT MsgBoxProc(HWND, uint, WPARAM, LPARAM);

static struct wclass *
find_class(const char *name)
{
  for(int i = 0; i < MAXCLASS; i++)
    if(classes[i].used && strcmp(classes[i].name, name) == 0)
      return &classes[i];
  return 0;
}

int
RegisterClass(const WNDCLASS *wc)
{
  if(find_class(wc->lpszClassName))
    return 0;
  for(int i = 0; i < MAXCLASS; i++)
    if(!classes[i].used){
      classes[i].used = 1;
      scopy(classes[i].name, wc->lpszClassName, sizeof(classes[i].name));
      classes[i].proc = wc->lpfnWndProc;
      classes[i].bg = wc->hbrBackground;
      return i + 1;
    }
  return 0;
}

static char *
strdup_(const char *s)
{
  int n = s ? strlen(s) : 0;
  char *d = malloc(n + 1);
  if(d){
    if(n)
      memmove(d, s, n);
    d[n] = 0;
  }
  return d;
}

static HWND
by_khwnd(uint32 h)
{
  for(int i = 0; i < MAXWIN; i++)
    if(windows[i].used && !windows[i].parent && windows[i].khwnd == h)
      return &windows[i];
  return 0;
}

HWND
CreateWindow(const char *cls, const char *title, uint style, int x, int y,
             int w, int h, HWND parent, int id, void *param)
{
  struct wclass *c = find_class(cls);
  struct window *win = 0;

  if(c == 0 || (parent && !valid(parent)))
    return 0;
  for(int i = 0; i < MAXWIN; i++)
    if(!windows[i].used){
      win = &windows[i];
      break;
    }
  if(win == 0)
    return 0;
  memset(win, 0, sizeof(*win));
  win->used = 1;
  win->cls = c;
  win->proc = c->proc;
  win->text = strdup_(title);
  win->style = style;
  win->id = id;
  if(!parent && x == CW_USEDEFAULT){
    // Spread new windows out by process, since each has its own cascade.
    static int cascade = -1;
    if(cascade < 0)
      cascade = getpid() % 7;
    x = 24 + cascade * 26;
    y = 16 + cascade * 22;
    cascade = (cascade + 1) % 7;
  }
  if(!parent){
    int work = SH - GetSystemMetrics(SM_CYTASKBAR);
    if(x + w > SW) x = SW - w > 0 ? SW - w : 0;
    if(y + h > work && !(style & WS_TOPMOST)) y = work - h > 0 ? work - h : 0;
  }
  win->rc.left = x;
  win->rc.top = y;
  win->rc.right = x + w;
  win->rc.bottom = y + h;
  win->visible = (style & WS_VISIBLE) != 0;
  if(parent){
    win->parent = parent;
    struct window **pp = &parent->child;
    while(*pp)
      pp = &(*pp)->next;
    *pp = win;
  }
  if(SendMessage(win, WM_CREATE, 0, (LPARAM)param) < 0){
    DestroyWindow(win);
    return 0;
  }
  if(!valid(win))
    return 0;
  if(!parent){
    // The kernel object is created after WM_CREATE so that the children
    // exist before the first WM_PAINT arrives.
    struct w32_create cr;
    memset(&cr, 0, sizeof(cr));
    cr.x = x; cr.y = y; cr.w = w; cr.h = h;
    cr.style = (win->visible ? W32S_VISIBLE : 0) |
               (HASCAP(style) ? W32S_CAPTION : 0) |
               ((style & WS_TOPMOST) ? W32S_TOPMOST : 0) |
               ((style & WS_BOTTOM) ? W32S_BOTTOM : 0) |
               ((style & WS_NOTASKBAR) ? W32S_NOTASKBAR : 0) |
               ((style & WS_MENU_INTERNAL) ? W32S_MENU : 0);
    scopy(cr.title, win->text ? win->text : "", sizeof(cr.title));
    win->khwnd = (uint32)k(W32_CREATE, (long)&cr, 0, 0);
    if(win->khwnd == 0 || win->khwnd == (uint32)-1){
      win->khwnd = 0;
      DestroyWindow(win);
      return 0;
    }
    if(HASCAP(style) && win->visible)
      active_top = win;
  } else if(win->visible){
    damage_window(win);
  }
  return win;
}

static void
purge(HWND w)
{
  for(int i = qhead; i != qtail; i = (i + 1) % MAXQUEUE)
    if(queue[i].hwnd == w)
      queue[i].hwnd = 0;
  for(int i = 0; i < MAXTIMER; i++)
    if(timers[i].hwnd == w)
      timers[i].hwnd = 0;
}

int
DestroyWindow(HWND w)
{
  if(!valid(w))
    return 0;
  if(w->parent)
    damage_window(w);
  while(w->child)
    DestroyWindow(w->child);
  SendMessage(w, WM_DESTROY, 0, 0);
  if(!valid(w))
    return 1;
  if(w->parent){
    struct window **pp = &w->parent->child;
    while(*pp && *pp != w)
      pp = &(*pp)->next;
    if(*pp)
      *pp = w->next;
    HWND t = toplevel(w);
    if(t && t->focus == w)
      t->focus = 0;
  } else if(w->khwnd){
    k(W32_DESTROY, w->khwnd, 0, 0);
  }
  purge(w);
  if(capture == w || (capture && toplevel(capture) == w)) capture = 0;
  if(popup == w) popup = 0;
  if(active_top == w) active_top = 0;
  free(w->text);
  free(w->ctl);
  w->used = 0;
  return 1;
}

int
ShowWindow(HWND w, int show)
{
  if(!valid(w))
    return 0;
  if(!w->parent){
    w->visible = 1;
    w->minimized = !show;
    if(w->khwnd)
      k(W32_SHOW, w->khwnd, show, 0);
  } else {
    damage_window(w);
    w->visible = show != 0;
    damage_window(w);
  }
  return 1;
}

int
MoveWindow(HWND w, int x, int y, int cx, int cy, int repaint)
{
  if(!valid(w))
    return 0;
  (void)repaint;
  if(!w->parent){
    struct w32_rect r = { x, y, x + cx, y + cy };
    w->rc.left = x; w->rc.top = y; w->rc.right = x + cx; w->rc.bottom = y + cy;
    return k(W32_MOVE, w->khwnd, (long)&r, 0) == 0;
  }
  damage_window(w);
  w->rc.left = x;
  w->rc.top = y;
  w->rc.right = x + cx;
  w->rc.bottom = y + cy;
  damage_window(w);
  return 1;
}

int
SetWindowText(HWND w, const char *s)
{
  if(!valid(w))
    return 0;
  SendMessage(w, WM_SETTEXT, 0, (LPARAM)s);
  if(!w->parent && w->khwnd)
    k(W32_SETTEXT, w->khwnd, (long)(s ? s : ""), 0);
  return 1;
}

static int edit_get(HWND w, char *buf, int max);
static int edit_len(HWND w);

int
GetWindowText(HWND w, char *buf, int max)
{
  if(!valid(w) || max <= 0)
    return 0;
  if(w->proc == EditProc)
    return edit_get(w, buf, max);
  scopy(buf, w->text ? w->text : "", max);
  return strlen(buf);
}

int
GetWindowTextLength(HWND w)
{
  if(!valid(w))
    return 0;
  if(w->proc == EditProc)
    return edit_len(w);
  return w->text ? strlen(w->text) : 0;
}

HWND
GetDlgItem(HWND p, int id)
{
  if(!valid(p))
    return 0;
  for(HWND c = p->child; c; c = c->next)
    if(c->id == id)
      return c;
  return 0;
}

long
GetWindowLong(HWND w, int i)
{
  if(!valid(w))
    return 0;
  return i == GWLP_USERDATA ? w->userdata : i == GWLP_ID ? w->id : 0;
}

long
SetWindowLong(HWND w, int i, long v)
{
  long old = GetWindowLong(w, i);
  if(valid(w) && i == GWLP_USERDATA)
    w->userdata = v;
  return old;
}

// Keyboard focus: per top-level window, as each process only ever sees the
// keys the kernel routes to its active window.
HWND
GetFocus(void)
{
  if(valid(popup))
    return popup;
  if(!valid(active_top))
    return 0;
  return valid(active_top->focus) ? active_top->focus : active_top;
}

HWND
SetFocus(HWND w)
{
  if(!valid(w))
    return 0;
  HWND t = toplevel(w), old = t->focus;
  if(old == w)
    return old;
  t->focus = w;
  if(valid(old)){
    SendMessage(old, WM_KILLFOCUS, (WPARAM)w, 0);
    damage_window(old);
  }
  SendMessage(w, WM_SETFOCUS, (WPARAM)old, 0);
  damage_window(w);
  return old;
}

HWND
GetActiveWindow(void)
{
  return valid(active_top) ? active_top : 0;
}

HWND
SetActiveWindow(HWND w)
{
  HWND old = active_top;
  if(!valid(w))
    return old;
  w = toplevel(w);
  if(w->khwnd)
    k(W32_ACTIVATE, w->khwnd, 0, 0);
  if(HASCAP(w->style) || (w->style & WS_BOTTOM))
    active_top = w;
  return old;
}

// ---------------------------------------------------------------------------
// Messages.

int
PostMessage(HWND w, uint msg, WPARAM wp, LPARAM lp)
{
  // A top-level window's queue is the kernel's: that is also safe from
  // another thread of this process (the console reader).
  if(valid(w) && !w->parent && w->khwnd){
    struct w32_msg m = { w->khwnd, msg, wp, lp };
    return k(W32_POST, (long)&m, 0, 0) > 0;
  }
  int next = (qtail + 1) % MAXQUEUE;
  if(next == qhead)
    return 0;
  queue[qtail].hwnd = w;
  queue[qtail].message = msg;
  queue[qtail].wParam = wp;
  queue[qtail].lParam = lp;
  qtail = next;
  return 1;
}

static void
post_local(HWND w, uint msg, WPARAM wp, LPARAM lp)
{
  int next = (qtail + 1) % MAXQUEUE;
  if(next == qhead)
    return;
  queue[qtail].hwnd = w;
  queue[qtail].message = msg;
  queue[qtail].wParam = wp;
  queue[qtail].lParam = lp;
  qtail = next;
}

LRESULT
SendMessage(HWND w, uint msg, WPARAM wp, LPARAM lp)
{
  if(!valid(w))
    return 0;
  return w->proc(w, msg, wp, lp);
}

void
PostQuitMessage(int code)
{
  quit_posted = 1;
  k(W32_QUIT, code, 0, 0);
}

LRESULT
DispatchMessage(const MSG *m)
{
  if(!valid(m->hwnd))
    return 0;
  return m->hwnd->proc(m->hwnd, m->message, m->wParam, m->lParam);
}

LRESULT
DefWindowProc(HWND w, uint msg, WPARAM wp, LPARAM lp)
{
  (void)wp;
  switch(msg){
  case WM_CLOSE:
    DestroyWindow(w);
    return 0;
  case WM_SETTEXT:
    free(w->text);
    w->text = strdup_((const char *)lp);
    damage_window(w);
    return 1;
  }
  return 0;
}

int
SetTimer(HWND w, int id, int ms)
{
  KillTimer(w, id);
  for(int i = 0; i < MAXTIMER; i++)
    if(!timers[i].hwnd){
      timers[i].hwnd = w;
      timers[i].id = id;
      timers[i].ms = ms < 10 ? 10 : ms;
      timers[i].next = clock_us() + timers[i].ms * 1000ULL;
      return id;
    }
  return 0;
}

int
KillTimer(HWND w, int id)
{
  for(int i = 0; i < MAXTIMER; i++)
    if(timers[i].hwnd == w && timers[i].id == id)
      timers[i].hwnd = 0;
  return 1;
}

// ---------------------------------------------------------------------------
// Other processes' windows (the taskbar).

int
WinEnumWindows(KWND *out, int max)
{
  long n = k(W32_ENUM, (long)out, max, 0);
  return n < 0 ? 0 : (int)n;
}

int
WinEnumAllWindows(KWND *out, int max)
{
  long n = k(W32_ENUM, (long)out, max, 1);
  return n < 0 ? 0 : (int)n;
}

int
KwndGetText(KWND h, char *buf, int max)
{
  long n = k(W32_GETTEXT, h, (long)buf, max);
  if(n < 0 && max > 0)
    buf[0] = 0;
  return n < 0 ? 0 : (int)n;
}

int
KwndGetInfo(KWND h, struct w32_info *info)
{
  return k(W32_GETINFO, h, (long)info, 0) == 0;
}

KWND
KwndGetActive(void)
{
  long h = k(W32_GETACTIVE, 0, 0, 0);
  return h < 0 ? 0 : (KWND)h;
}

void KwndActivate(KWND h) { k(W32_ACTIVATE, h, 0, 0); }
void KwndShow(KWND h, int show) { k(W32_SHOW, h, show, 0); }

void
KwndPost(KWND h, uint msg, WPARAM wp, LPARAM lp)
{
  struct w32_msg m = { h, msg, wp, lp };
  k(W32_POST, (long)&m, 0, 0);
}

KWND WinGetKwnd(HWND w) { return valid(w) ? toplevel(w)->khwnd : 0; }

void
WinSetShell(HWND w)
{
  if(valid(w) && w->khwnd)
    k(W32_SETSHELL, w->khwnd, 0, 0);
}

// ---------------------------------------------------------------------------
// Painting.

static struct dc paint_dc;
static RECT paint_rect;

HDC
BeginPaint(HWND w, PAINTSTRUCT *ps)
{
  (void)w;
  ps->hdc = &paint_dc;
  ps->rcPaint = paint_rect;
  return &paint_dc;
}

int EndPaint(HWND w, const PAINTSTRUCT *ps) { (void)w; (void)ps; return 1; }

static struct dc get_dc;

HDC
GetDC(HWND w)
{
  if(!valid(w))
    return 0;
  get_dc.hwnd = w;
  client_origin(w, &get_dc.ox, &get_dc.oy);
  client_screen_rect(w, &get_dc.clip);
  get_dc.text = CLR_TEXT;
  get_dc.bk = CLR_WINDOW;
  get_dc.opaque = 0;
  return &get_dc;
}

int
ReleaseDC(HWND w, HDC dc)
{
  // Drawing outside WM_PAINT is not supported on the shared screen: ask
  // the kernel for a WM_PAINT of the window instead.
  (void)dc;
  InvalidateRect(w, 0, 1);
  return 1;
}

int
FillRect(HDC dc, const RECT *r, COLORREF c)
{
  sfill(&dc->clip, dc->ox + r->left, dc->oy + r->top, dc->ox + r->right,
        dc->oy + r->bottom, c);
  return 1;
}

int
FrameRect(HDC dc, const RECT *r, COLORREF c)
{
  RECT t;
  t = *r; t.bottom = t.top + 1; FillRect(dc, &t, c);
  t = *r; t.top = t.bottom - 1; FillRect(dc, &t, c);
  t = *r; t.right = t.left + 1; FillRect(dc, &t, c);
  t = *r; t.left = t.right - 1; FillRect(dc, &t, c);
  return 1;
}

int
SetPixel(HDC dc, int x, int y, COLORREF c)
{
  sfill(&dc->clip, dc->ox + x, dc->oy + y, dc->ox + x + 1, dc->oy + y + 1, c);
  return 1;
}

COLORREF SetTextColor(HDC dc, COLORREF c) { COLORREF o = dc->text; dc->text = c; return o; }
COLORREF SetBkColor(HDC dc, COLORREF c) { COLORREF o = dc->bk; dc->bk = c; return o; }
int SetBkMode(HDC dc, int opaque) { int o = dc->opaque; dc->opaque = opaque; return o; }
int GetTextWidth(const char *s) { return strlen(s) * FONT_W; }

int
TextOut(HDC dc, int x, int y, const char *s, int n)
{
  if(n < 0)
    n = strlen(s);
  if(dc->opaque)
    sfill(&dc->clip, dc->ox + x, dc->oy + y, dc->ox + x + n * FONT_W,
          dc->oy + y + FONT_H, dc->bk);
  stext(&dc->clip, dc->ox + x, dc->oy + y, s, n, dc->text, 0);
  return 1;
}

int
DrawText(HDC dc, const char *s, int n, const RECT *r, uint fmt)
{
  if(n < 0)
    n = strlen(s);
  int w = n * FONT_W, x = r->left;
  if(fmt == DT_CENTER)
    x = r->left + (r->right - r->left - w) / 2;
  else if(fmt == DT_RIGHT)
    x = r->right - w;
  int y = r->top + (r->bottom - r->top - FONT_H) / 2;
  struct dc sub = *dc;
  RECT cr = { dc->ox + r->left, dc->oy + r->top, dc->ox + r->right,
              dc->oy + r->bottom };
  rect_intersect(&sub.clip, &dc->clip, &cr);
  return TextOut(&sub, x, y, s, n);
}

int
DrawEdge(HDC dc, const RECT *r, int edge)
{
  RECT s = { dc->ox + r->left, dc->oy + r->top, dc->ox + r->right,
             dc->oy + r->bottom };
  sedge(&dc->clip, s, edge);
  return 1;
}

static void
draw_caption_button(const RECT *clip, int x, int y, int kind, int pressed)
{
  RECT b = { x, y, x + 16, y + 14 };
  sfill(clip, b.left, b.top, b.right, b.bottom, CLR_FACE);
  sedge(clip, b, pressed ? EDGE_SUNKEN : EDGE_RAISED);
  int o = pressed ? 1 : 0;
  if(kind == 1){                // minimize: a bar at the bottom
    sfill(clip, x + 4 + o, y + 9 + o, x + 10 + o, y + 11 + o, CLR_TEXT);
  } else {                      // close: an X
    for(int i = 0; i < 7; i++){
      sfill(clip, x + 4 + i + o, y + 3 + i + o, x + 6 + i + o, y + 4 + i + o, CLR_TEXT);
      sfill(clip, x + 10 - i + o, y + 3 + i + o, x + 12 - i + o, y + 4 + i + o, CLR_TEXT);
    }
  }
}

static void
caption_buttons(HWND w, RECT *min, RECT *close)
{
  int y = w->rc.top + FRAME + 2;
  close->left = w->rc.right - FRAME - 2 - 16;
  close->top = y; close->right = close->left + 16; close->bottom = y + 14;
  min->left = close->left - 18;
  min->top = y; min->right = min->left + 16; min->bottom = y + 14;
}

static void
paint_frame(HWND w, const RECT *clip)
{
  RECT r = w->rc;
  sfill(clip, r.left, r.top, r.right, r.bottom, CLR_FACE);
  sedge(clip, r, EDGE_RAISED);
  int is_active = active == w;
  RECT cap = { r.left + FRAME, r.top + FRAME, r.right - FRAME,
               r.top + FRAME + CAPTION_H };
  // Horizontal gradient, as on the classic theme.
  int width = cap.right - cap.left;
  for(int x = cap.left; x < cap.right; x++){
    int t = (x - cap.left) * 256 / (width ? width : 1);
    COLORREF c = is_active ?
      RGB((0 * (256 - t) + 16 * t) >> 8, (0 * (256 - t) + 132 * t) >> 8,
          (128 * (256 - t) + 208 * t) >> 8) :
      RGB((128 * (256 - t) + 192 * t) >> 8, (128 * (256 - t) + 192 * t) >> 8,
          (128 * (256 - t) + 192 * t) >> 8);
    sfill(clip, x, cap.top, x + 1, cap.bottom - 1, c);
  }
  RECT tclip;
  RECT tr = { cap.left, cap.top, cap.right - 40, cap.bottom };
  rect_intersect(&tclip, clip, &tr);
  // A tiny window icon, then the title.
  sfill(&tclip, cap.left + 3, cap.top + 3, cap.left + 14, cap.top + 14,
        CLR_HILIGHT);
  sfill(&tclip, cap.left + 3, cap.top + 3, cap.left + 14, cap.top + 6,
        RGB(0, 0, 128));
  if(w->text)
    stext(&tclip, cap.left + 18, cap.top + 1, w->text, strlen(w->text),
          is_active ? CLR_HILIGHT : CLR_LIGHT, 1);
  RECT mn, cl;
  caption_buttons(w, &mn, &cl);
  draw_caption_button(clip, mn.left, mn.top, 1,
                      nc_button_win == w && nc_button == 1);
  draw_caption_button(clip, cl.left, cl.top, 2,
                      nc_button_win == w && nc_button == 2);
}

static void
paint_tree(HWND w, const RECT *parent_clip)
{
  RECT wr, clip, cr, cclip;
  if(!w->visible || w->minimized)
    return;
  window_screen_rect(w, &wr);
  rect_intersect(&clip, parent_clip, &wr);
  if(rect_empty(&clip))
    return;
  if(HASCAP(w->style))
    paint_frame(w, &clip);
  else if(w->style & WS_BORDER)
    sedge(&clip, wr, EDGE_SUNKEN);
  client_screen_rect(w, &cr);
  rect_intersect(&cclip, &clip, &cr);
  if(rect_empty(&cclip))
    return;
  if(w->cls->bg != (COLORREF)-1)
    sfill(&cclip, cr.left, cr.top, cr.right, cr.bottom, w->cls->bg);
  paint_dc.hwnd = w;
  paint_dc.ox = cr.left;
  paint_dc.oy = cr.top;
  paint_dc.clip = cclip;
  paint_dc.text = CLR_TEXT;
  paint_dc.bk = w->cls->bg;
  paint_dc.opaque = 0;
  paint_rect.left = cclip.left - cr.left;
  paint_rect.top = cclip.top - cr.top;
  paint_rect.right = cclip.right - cr.left;
  paint_rect.bottom = cclip.bottom - cr.top;
  w->proc(w, WM_PAINT, 0, 0);
  for(HWND c = w->child; c; c = c->next)
    paint_tree(c, &cclip);
}


// ---------------------------------------------------------------------------
// Painting a top-level window when the kernel asks for it.

static void
paint_toplevel(HWND w)
{
  struct w32_paint ps;
  if(k(W32_BEGINPAINT, w->khwnd, (long)&ps, 0) < 0)
    return;
  w->rc.left = ps.rc.left;
  w->rc.top = ps.rc.top;
  w->rc.right = ps.rc.right;
  w->rc.bottom = ps.rc.bottom;
  if(ps.nvis > 0){
    RECT clip = { ps.rc.left + ps.update.left, ps.rc.top + ps.update.top,
                  ps.rc.left + ps.update.right, ps.rc.top + ps.update.bottom };
    RECT screen = { 0, 0, SW, SH };
    rect_intersect(&clip, &clip, &screen);
    active = ps.active ? w : 0;
    paint_tree(w, &clip);
    active = 0;
    // Only the parts nobody covers reach the screen.
    for(int i = 0; i < ps.nvis; i++){
      RECT v = { ps.vis[i].left, ps.vis[i].top, ps.vis[i].right,
                 ps.vis[i].bottom };
      rect_intersect(&v, &v, &screen);
      for(int y = v.top; y < v.bottom; y++)
        memmove(fb + y * stride + v.left, bb + y * SW + v.left,
                (v.right - v.left) * 4);
    }
  }
  k(W32_ENDPAINT, w->khwnd, 0, 0);
}

// ---------------------------------------------------------------------------
// The message loop.

static HWND
deepest_child(HWND w, int x, int y)
{
  for(;;){
    HWND hit = 0;
    RECT cr;
    client_screen_rect(w, &cr);
    if(!pt_in(&cr, x, y))
      return w;
    for(HWND c = w->child; c; c = c->next){
      RECT r;
      if(!c->visible)
        continue;
      window_screen_rect(c, &r);
      if(pt_in(&r, x, y))
        hit = c;                // later siblings are on top
    }
    if(!hit)
      return w;
    w = hit;
  }
}

static LPARAM
client_pos(HWND w, int x, int y)
{
  int ox, oy;
  client_origin(w, &ox, &oy);
  return MAKELPARAM(x - ox, y - oy);
}

// A kernel message for one of our top-level windows: route it to the
// window procedure that should see it (a child control for mouse and keys).
static void
translate(struct w32_msg *km)
{
  HWND t = km->hwnd ? by_khwnd(km->hwnd) : 0;
  if(!t)
    return;
  switch(km->message){
  case W32M_PAINT:
    paint_toplevel(t);
    return;
  case W32M_ACTIVATE:
    if(km->wparam)
      active_top = t;
    else if(active_top == t)
      active_top = 0;
    return;
  case W32M_CANCELMODE:
    if(t == popup)
      DestroyWindow(t);
    return;
  case W32M_MOUSEMOVE:
  case W32M_LBUTTONDOWN:
  case W32M_LBUTTONUP:
  case W32M_LBUTTONDBLCLK:
  case W32M_RBUTTONDOWN:
  case W32M_MOUSEWHEEL: {
    int sx = t->rc.left + LOWORD(km->lparam);
    int sy = t->rc.top + HIWORD(km->lparam);
    HWND target;
    if(valid(capture) && toplevel(capture) == t &&
       (km->message == W32M_MOUSEMOVE || km->message == W32M_LBUTTONUP))
      target = capture;
    else
      target = deepest_child(t, sx, sy);
    if(km->message == W32M_LBUTTONDOWN || km->message == W32M_LBUTTONDBLCLK){
      if(target != popup)
        SetFocus(target);
      capture = target;
    }
    post_local(target, km->message, km->wparam, client_pos(target, sx, sy));
    if(km->message == W32M_LBUTTONUP)
      capture = 0;
    return;
  }
  case W32M_KEYDOWN:
  case W32M_CHAR: {
    HWND target = t == popup ? t : (valid(t->focus) ? t->focus : t);
    post_local(target, km->message, km->wparam, km->lparam);
    return;
  }
  }
  post_local(t, km->message, km->wparam, km->lparam);   // WM_CLOSE etc.
}

int
GetMessage(MSG *m, HWND filter, uint min, uint max)
{
  (void)filter; (void)min; (void)max;
  for(;;){
    while(qhead != qtail){
      *m = queue[qhead];
      qhead = (qhead + 1) % MAXQUEUE;
      if(valid(m->hwnd))
        return 1;
    }
    if(quit_posted){
      m->hwnd = 0;
      m->message = WM_QUIT;
      return 0;
    }
    uint64 now = clock_us(), next = 0;
    int fired = 0;
    for(int i = 0; i < MAXTIMER; i++){
      if(!timers[i].hwnd)
        continue;
      if(timers[i].next <= now){
        post_local(timers[i].hwnd, WM_TIMER, timers[i].id, 0);
        timers[i].next = now + timers[i].ms * 1000ULL;
        fired = 1;
      }
      if(!next || timers[i].next < next)
        next = timers[i].next;
    }
    if(fired)
      continue;
    int timeout = -1;
    if(next)
      timeout = next > now ? (int)((next - now) / 1000) + 1 : 1;
    struct w32_msg km;
    long r = k(W32_GETMSG, (long)&km, timeout, 0);
    if(r == 0 || r < 0){        // WM_QUIT, or the process is being killed
      m->hwnd = 0;
      m->message = WM_QUIT;
      return 0;
    }
    if(r == 1)
      translate(&km);
  }
}

// ---------------------------------------------------------------------------
// Built-in controls.

// BUTTON ---------------------------------------------------------------
static LRESULT
ButtonProc(HWND w, uint msg, WPARAM wp, LPARAM lp)
{
  long *pressed = &w->userdata;
  switch(msg){
  case WM_LBUTTONDOWN:
  case WM_LBUTTONDBLCLK:
    *pressed = 1;
    InvalidateRect(w, 0, 1);
    return 0;
  case WM_MOUSEMOVE:
    if(capture == w){
      RECT r;
      GetClientRect(w, &r);
      int in = pt_in(&r, LOWORD(lp), HIWORD(lp));
      if(in != *pressed){
        *pressed = in;
        InvalidateRect(w, 0, 1);
      }
    }
    return 0;
  case WM_LBUTTONUP:
    if(*pressed){
      *pressed = 0;
      InvalidateRect(w, 0, 1);
      if(valid(w->parent))
        SendMessage(w->parent, WM_COMMAND, MAKELPARAM(w->id, BN_CLICKED),
                    (LPARAM)w);
    }
    return 0;
  case WM_KEYDOWN:
    if(wp == ' ' || wp == VK_RETURN)
      SendMessage(w->parent, WM_COMMAND, MAKELPARAM(w->id, BN_CLICKED),
                  (LPARAM)w);
    return 0;
  case WM_PAINT: {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(w, &ps);
    RECT r;
    GetClientRect(w, &r);
    FillRect(dc, &r, CLR_FACE);
    if(*pressed){
      FrameRect(dc, &r, CLR_DKSHADOW);
      RECT in = { 1, 1, r.right - 1, r.bottom - 1 };
      FrameRect(dc, &in, CLR_SHADOW);
    } else {
      DrawEdge(dc, &r, EDGE_RAISED);
    }
    RECT t = r;
    if(*pressed){ t.left += 2; t.top += 2; }
    if(w->text)
      DrawText(dc, w->text, -1, &t, DT_CENTER);
    if(GetFocus() == w){
      RECT f = { 4, 4, r.right - 4, r.bottom - 4 };
      for(int x = f.left; x < f.right; x += 2){
        SetPixel(dc, x, f.top, CLR_TEXT);
        SetPixel(dc, x, f.bottom - 1, CLR_TEXT);
      }
      for(int y = f.top; y < f.bottom; y += 2){
        SetPixel(dc, f.left, y, CLR_TEXT);
        SetPixel(dc, f.right - 1, y, CLR_TEXT);
      }
    }
    EndPaint(w, &ps);
    return 0;
  }
  }
  return DefWindowProc(w, msg, wp, lp);
}

// STATIC ---------------------------------------------------------------
static LRESULT
StaticProc(HWND w, uint msg, WPARAM wp, LPARAM lp)
{
  if(msg == WM_PAINT){
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(w, &ps);
    RECT r;
    GetClientRect(w, &r);
    if(w->style & ES_READONLY)          // "display" look: white well
      FillRect(dc, &r, CLR_WINDOW);
    if(w->text){
      // One line per '\n'.
      const char *s = w->text;
      int y = 0;
      while(*s){
        int n = 0;
        while(s[n] && s[n] != '\n')
          n++;
        RECT lr = { 2, y, r.right - 2, y + FONT_H };
        DrawText(dc, s, n, &lr, (w->style & ES_RIGHT) ? DT_RIGHT : DT_LEFT);
        y += FONT_H + 2;
        s += n;
        if(*s == '\n')
          s++;
      }
    }
    EndPaint(w, &ps);
    return 0;
  }
  return DefWindowProc(w, msg, wp, lp);
}

// EDIT -----------------------------------------------------------------
struct edit {
  char *buf;
  int len, cap;
  int caret;
  int top;                      // first visible line (multiline)
  int left;                     // first visible column (single line)
};

static struct edit *
edit_of(HWND w)
{
  return (struct edit *)w->ctl;
}

static int
edit_reserve(struct edit *e, int need)
{
  if(need + 1 <= e->cap)
    return 0;
  int cap = e->cap ? e->cap : 256;
  while(cap < need + 1)
    cap *= 2;
  char *nb = malloc(cap);
  if(!nb)
    return -1;
  if(e->buf){
    memmove(nb, e->buf, e->len);
    free(e->buf);
  }
  e->buf = nb;
  e->cap = cap;
  return 0;
}

static int
edit_get(HWND w, char *buf, int max)
{
  struct edit *e = edit_of(w);
  int n = e->len < max - 1 ? e->len : max - 1;
  memmove(buf, e->buf, n);
  buf[n] = 0;
  return n;
}

static int edit_len(HWND w) { return edit_of(w)->len; }

static void
edit_rowcol(struct edit *e, int pos, int *row, int *col)
{
  int r = 0, c = 0;
  for(int i = 0; i < pos && i < e->len; i++){
    if(e->buf[i] == '\n'){ r++; c = 0; } else c++;
  }
  *row = r;
  *col = c;
}

static int
edit_pos(struct edit *e, int row, int col)
{
  int r = 0, i = 0;
  while(i < e->len && r < row){
    if(e->buf[i] == '\n')
      r++;
    i++;
  }
  int c = 0;
  while(i < e->len && e->buf[i] != '\n' && c < col){
    i++;
    c++;
  }
  return i;
}

static void
edit_scroll_to_caret(HWND w)
{
  struct edit *e = edit_of(w);
  RECT r;
  GetClientRect(w, &r);
  int rows = (r.bottom - 4) / FONT_H, cols = (r.right - 6) / FONT_W;
  int row, col;
  edit_rowcol(e, e->caret, &row, &col);
  if(w->style & ES_MULTILINE){
    if(row < e->top) e->top = row;
    if(rows > 0 && row >= e->top + rows) e->top = row - rows + 1;
  } else {
    if(col < e->left) e->left = col;
    if(cols > 0 && col >= e->left + cols) e->left = col - cols + 1;
  }
}

static void
edit_changed(HWND w)
{
  edit_scroll_to_caret(w);
  InvalidateRect(w, 0, 1);
  if(valid(w->parent))
    SendMessage(w->parent, WM_COMMAND, MAKELPARAM(w->id, EN_CHANGE),
                (LPARAM)w);
}

static LRESULT
EditProc(HWND w, uint msg, WPARAM wp, LPARAM lp)
{
  struct edit *e = edit_of(w);
  int multi = (w->style & ES_MULTILINE) != 0, ro = (w->style & ES_READONLY) != 0;

  switch(msg){
  case WM_CREATE:
    w->ctl = malloc(sizeof(struct edit));
    if(!w->ctl)
      return -1;
    memset(w->ctl, 0, sizeof(struct edit));
    e = edit_of(w);
    edit_reserve(e, 0);
    if(w->text){
      int n = strlen(w->text);
      edit_reserve(e, n);
      memmove(e->buf, w->text, n);
      e->len = n;
    }
    return 0;
  case WM_SETTEXT: {
    const char *s = (const char *)lp;
    int n = s ? strlen(s) : 0;
    if(edit_reserve(e, n) < 0)
      return 0;
    memmove(e->buf, s, n);
    e->len = n;
    e->caret = 0;
    e->top = e->left = 0;
    InvalidateRect(w, 0, 1);
    return 1;
  }
  case WM_SETFOCUS:
  case WM_KILLFOCUS:
    InvalidateRect(w, 0, 1);
    return 0;
  case WM_LBUTTONDOWN:
  case WM_LBUTTONDBLCLK: {
    int col = (LOWORD(lp) - 3 + FONT_W / 2) / FONT_W;
    int row = (HIWORD(lp) - 2) / FONT_H;
    if(col < 0) col = 0;
    if(row < 0) row = 0;
    e->caret = multi ? edit_pos(e, e->top + row, col) :
                       edit_pos(e, 0, e->left + col);
    InvalidateRect(w, 0, 1);
    return 0;
  }
  case WM_MOUSEWHEEL:
    if(multi){
      e->top -= (int)(long)wp * 3;
      if(e->top < 0) e->top = 0;
      InvalidateRect(w, 0, 1);
    }
    return 0;
  case WM_KEYDOWN: {
    int row, col;
    edit_rowcol(e, e->caret, &row, &col);
    switch(wp){
    case VK_LEFT: if(e->caret > 0) e->caret--; break;
    case VK_RIGHT: if(e->caret < e->len) e->caret++; break;
    case VK_UP: if(multi && row > 0) e->caret = edit_pos(e, row - 1, col); break;
    case VK_DOWN: if(multi) e->caret = edit_pos(e, row + 1, col); break;
    case VK_HOME: e->caret = edit_pos(e, row, 0); break;
    case VK_END: e->caret = edit_pos(e, row, 1 << 20); break;
    case VK_DELETE:
      if(!ro && e->caret < e->len){
        memmove(e->buf + e->caret, e->buf + e->caret + 1, e->len - e->caret - 1);
        e->len--;
        edit_changed(w);
      }
      return 0;
    default:
      return 0;
    }
    edit_scroll_to_caret(w);
    InvalidateRect(w, 0, 1);
    return 0;
  }
  case WM_CHAR: {
    int c = wp;
    if(ro)
      return 0;
    if(c == '\b'){
      if(e->caret > 0){
        memmove(e->buf + e->caret - 1, e->buf + e->caret, e->len - e->caret);
        e->len--;
        e->caret--;
        edit_changed(w);
      }
      return 0;
    }
    if(c == '\r'){
      if(!multi){
        if(valid(w->parent))
          SendMessage(w->parent, WM_COMMAND, MAKELPARAM(w->id, VK_RETURN),
                      (LPARAM)w);
        return 0;
      }
      c = '\n';
    }
    if(c == '\t')
      c = ' ';
    if((c < ' ' || c > '~') && c != '\n')
      return 0;
    if(edit_reserve(e, e->len + 1) < 0)
      return 0;
    memmove(e->buf + e->caret + 1, e->buf + e->caret, e->len - e->caret);
    e->buf[e->caret++] = c;
    e->len++;
    edit_changed(w);
    return 0;
  }
  case WM_PAINT: {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(w, &ps);
    RECT r;
    GetClientRect(w, &r);
    FillRect(dc, &r, ro && !multi ? CLR_FACE : CLR_WINDOW);
    int rows = multi ? (r.bottom - 2) / FONT_H : 1;
    int y0 = multi ? 2 : (r.bottom - FONT_H) / 2;
    int line = 0, i = 0;
    while(line < e->top && i < e->len){
      if(e->buf[i] == '\n')
        line++;
      i++;
    }
    for(int vr = 0; vr < rows && i <= e->len; vr++){
      int start = i;
      while(i < e->len && e->buf[i] != '\n')
        i++;
      int skip = multi ? 0 : e->left;
      int n = i - start - skip;
      if(n > 0)
        TextOut(dc, 3, y0 + vr * FONT_H, e->buf + start + skip, n);
      if(i >= e->len)
        break;
      i++;                      // past '\n'
    }
    if(GetFocus() == w && !ro){
      int row, col;
      edit_rowcol(e, e->caret, &row, &col);
      int cx = 3 + (col - (multi ? 0 : e->left)) * FONT_W;
      int cy = y0 + (row - (multi ? e->top : 0)) * FONT_H;
      if(!multi || (row >= e->top && row < e->top + rows)){
        RECT cr = { cx, cy, cx + 1, cy + FONT_H };
        FillRect(dc, &cr, CLR_TEXT);
      }
    }
    EndPaint(w, &ps);
    return 0;
  }
  case WM_DESTROY:
    if(e)
      free(e->buf);
    return 0;
  }
  return DefWindowProc(w, msg, wp, lp);
}

// LISTBOX --------------------------------------------------------------
struct listbox {
  char **items;
  int n, cap;
  int sel, top;
};

#define LB_ROW (FONT_H + 1)
#define LB_SB  14

static int
lb_rows(HWND w)
{
  RECT r;
  GetClientRect(w, &r);
  return r.bottom / LB_ROW;
}

static void
lb_clamp(HWND w, struct listbox *l)
{
  int rows = lb_rows(w);
  if(l->top > l->n - rows) l->top = l->n - rows;
  if(l->top < 0) l->top = 0;
}

static void
lb_show_sel(HWND w, struct listbox *l)
{
  int rows = lb_rows(w);
  if(l->sel < 0)
    return;
  if(l->sel < l->top) l->top = l->sel;
  if(rows > 0 && l->sel >= l->top + rows) l->top = l->sel - rows + 1;
}

static void
lb_notify(HWND w, int code)
{
  if(valid(w->parent))
    SendMessage(w->parent, WM_COMMAND, MAKELPARAM(w->id, code), (LPARAM)w);
}

static LRESULT
ListBoxProc(HWND w, uint msg, WPARAM wp, LPARAM lp)
{
  struct listbox *l = (struct listbox *)w->ctl;
  switch(msg){
  case WM_CREATE:
    w->ctl = malloc(sizeof(struct listbox));
    if(!w->ctl)
      return -1;
    memset(w->ctl, 0, sizeof(struct listbox));
    ((struct listbox *)w->ctl)->sel = -1;
    return 0;
  case LB_ADDSTRING: {
    if(l->n == l->cap){
      int cap = l->cap ? l->cap * 2 : 32;
      char **ni = malloc(cap * sizeof(char *));
      if(!ni)
        return LB_ERR;
      if(l->items){
        memmove(ni, l->items, l->n * sizeof(char *));
        free(l->items);
      }
      l->items = ni;
      l->cap = cap;
    }
    l->items[l->n] = strdup_((const char *)lp);
    InvalidateRect(w, 0, 1);
    return l->n++;
  }
  case LB_RESETCONTENT:
    for(int i = 0; i < l->n; i++)
      free(l->items[i]);
    l->n = 0;
    l->sel = -1;
    l->top = 0;
    InvalidateRect(w, 0, 1);
    return 0;
  case LB_SETCURSEL:
    l->sel = (int)wp < l->n ? (int)wp : -1;
    lb_show_sel(w, l);
    InvalidateRect(w, 0, 1);
    return l->sel;
  case LB_GETCURSEL:
    return l->sel;
  case LB_GETCOUNT:
    return l->n;
  case LB_GETTEXT:
    if((int)wp < 0 || (int)wp >= l->n)
      return LB_ERR;
    strcpy((char *)lp, l->items[wp]);
    return strlen(l->items[wp]);
  case WM_LBUTTONDOWN:
  case WM_LBUTTONDBLCLK: {
    RECT r;
    GetClientRect(w, &r);
    int x = LOWORD(lp), y = HIWORD(lp), rows = lb_rows(w);
    if(l->n > rows && x >= r.right - LB_SB){
      l->top += y < r.bottom / 2 ? -rows : rows;   // page up / down
      lb_clamp(w, l);
      InvalidateRect(w, 0, 1);
      return 0;
    }
    int i = l->top + y / LB_ROW;
    if(i >= 0 && i < l->n){
      if(i != l->sel){
        l->sel = i;
        lb_notify(w, LBN_SELCHANGE);
      }
      if(msg == WM_LBUTTONDBLCLK)
        lb_notify(w, LBN_DBLCLK);
    }
    InvalidateRect(w, 0, 1);
    return 0;
  }
  case WM_MOUSEWHEEL:
    l->top -= (int)(long)wp * 3;
    lb_clamp(w, l);
    InvalidateRect(w, 0, 1);
    return 0;
  case WM_KEYDOWN:
    if(wp == VK_UP && l->sel > 0){ l->sel--; lb_notify(w, LBN_SELCHANGE); }
    else if(wp == VK_DOWN && l->sel + 1 < l->n){ l->sel++; lb_notify(w, LBN_SELCHANGE); }
    else if(wp == VK_RETURN && l->sel >= 0) lb_notify(w, LBN_DBLCLK);
    lb_show_sel(w, l);
    InvalidateRect(w, 0, 1);
    return 0;
  case WM_CHAR:
    if(wp == '\r' && l->sel >= 0)
      lb_notify(w, LBN_DBLCLK);
    return 0;
  case WM_PAINT: {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(w, &ps);
    RECT r;
    GetClientRect(w, &r);
    int rows = lb_rows(w), sb = l->n > rows;
    int width = r.right - (sb ? LB_SB : 0);
    for(int k = 0; k < rows && l->top + k < l->n; k++){
      int i = l->top + k;
      RECT row = { 0, k * LB_ROW, width, (k + 1) * LB_ROW };
      if(i == l->sel){
        FillRect(dc, &row, GetFocus() == w ? CLR_SELECT : CLR_SHADOW);
        SetTextColor(dc, CLR_HILIGHT);
      } else {
        SetTextColor(dc, CLR_TEXT);
      }
      TextOut(dc, 3, row.top, l->items[i], -1);
    }
    if(sb){
      RECT bar = { r.right - LB_SB, 0, r.right, r.bottom };
      FillRect(dc, &bar, CLR_LIGHT);
      int th = r.bottom * rows / l->n, ty = r.bottom * l->top / l->n;
      if(th < 10) th = 10;
      if(ty + th > r.bottom) ty = r.bottom - th;
      RECT thumb = { r.right - LB_SB, ty, r.right, ty + th };
      FillRect(dc, &thumb, CLR_FACE);
      DrawEdge(dc, &thumb, EDGE_RAISED);
    }
    EndPaint(w, &ps);
    return 0;
  }
  case WM_DESTROY:
    if(l){
      for(int i = 0; i < l->n; i++)
        free(l->items[i]);
      free(l->items);
    }
    return 0;
  }
  return DefWindowProc(w, msg, wp, lp);
}


// POPUP MENU -----------------------------------------------------------
struct menu {
  const char **items;
  int n, hot, base;
  HWND owner;
};

#define MENU_ROW 20
#define MENU_SEP 8

static int
menu_item_at(struct menu *m, int y)
{
  int top = 2;
  for(int i = 0; i < m->n; i++){
    int h = m->items[i] ? MENU_ROW : MENU_SEP;
    if(y >= top && y < top + h)
      return m->items[i] ? i : -1;
    top += h;
  }
  return -1;
}

static LRESULT
MenuProc(HWND w, uint msg, WPARAM wp, LPARAM lp)
{
  struct menu *m = (struct menu *)w->ctl;
  switch(msg){
  case WM_MOUSEMOVE: {
    int hot = menu_item_at(m, HIWORD(lp));
    if(hot != m->hot){
      m->hot = hot;
      InvalidateRect(w, 0, 1);
    }
    return 0;
  }
  case WM_LBUTTONUP: {
    int i = menu_item_at(m, HIWORD(lp));
    if(i >= 0){
      HWND owner = m->owner;
      int id = m->base + i;
      DestroyWindow(w);
      PostMessage(owner, WM_COMMAND, MAKELPARAM(id, 0), 0);
    }
    return 0;
  }
  case WM_KEYDOWN:
    if(wp == VK_ESCAPE){
      DestroyWindow(w);
    } else if(wp == VK_DOWN || wp == VK_UP){
      int d = wp == VK_DOWN ? 1 : -1, i = m->hot;
      for(int k = 0; k < m->n; k++){
        i = (i + d + m->n) % m->n;
        if(m->items[i])
          break;
      }
      m->hot = i;
      InvalidateRect(w, 0, 1);
    } else if(wp == VK_RETURN && m->hot >= 0){
      HWND owner = m->owner;
      int id = m->base + m->hot;
      DestroyWindow(w);
      PostMessage(owner, WM_COMMAND, MAKELPARAM(id, 0), 0);
    }
    return 0;
  case WM_CHAR:
    if(wp == 0x1b)
      DestroyWindow(w);
    return 0;
  case WM_PAINT: {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(w, &ps);
    RECT r;
    GetClientRect(w, &r);
    FillRect(dc, &r, CLR_FACE);
    DrawEdge(dc, &r, EDGE_RAISED);
    // A vertical banner on the left, like the classic Start menu.
    RECT banner = { 3, 3, 23, r.bottom - 3 };
    FillRect(dc, &banner, RGB(0, 0, 128));
    const char *word = "xv6";
    for(int i = 0; word[i]; i++){
      SetTextColor(dc, CLR_HILIGHT);
      TextOut(dc, 9, r.bottom - 8 - (3 - i) * FONT_H, word + i, 1);
    }
    int top = 2;
    for(int i = 0; i < m->n; i++){
      if(!m->items[i]){
        RECT sep = { 28, top + 3, r.right - 4, top + 5 };
        DrawEdge(dc, &sep, EDGE_ETCHED);
        top += MENU_SEP;
        continue;
      }
      RECT row = { 26, top, r.right - 3, top + MENU_ROW };
      if(i == m->hot){
        FillRect(dc, &row, CLR_SELECT);
        SetTextColor(dc, CLR_HILIGHT);
      } else {
        SetTextColor(dc, CLR_TEXT);
      }
      TextOut(dc, 34, top + 2, m->items[i], -1);
      top += MENU_ROW;
    }
    EndPaint(w, &ps);
    return 0;
  }
  }
  return DefWindowProc(w, msg, wp, lp);
}

HWND
TrackPopupMenu(const char **items, int n, int x, int y, HWND owner,
               int base_id)
{
  if(valid(popup))
    DestroyWindow(popup);
  int wmax = 0, h = 4;
  for(int i = 0; i < n; i++){
    if(items[i]){
      int l = strlen(items[i]);
      if(l > wmax) wmax = l;
      h += MENU_ROW;
    } else {
      h += MENU_SEP;
    }
  }
  int w = 34 + wmax * FONT_W + 16;
  if(y + h > SH) y -= h;        // no room below (the Start button): open upward
  if(y < 0) y = 0;
  if(x + w > SW) x = SW - w;
  HWND m = CreateWindow("#menu", "", WS_POPUP | WS_TOPMOST | WS_NOTASKBAR |
                        WS_VISIBLE | WS_MENU_INTERNAL, x, y, w, h, 0, 0, 0);
  if(!m)
    return 0;
  struct menu *st = malloc(sizeof(*st));
  if(!st){
    DestroyWindow(m);
    return 0;
  }
  st->items = items;
  st->n = n;
  st->hot = -1;
  st->base = base_id;
  st->owner = owner;
  m->ctl = st;
  popup = m;                    // the kernel sends it the keys while open
  return m;
}

// MESSAGE BOX ----------------------------------------------------------
static LRESULT
MsgBoxProc(HWND w, uint msg, WPARAM wp, LPARAM lp)
{
  switch(msg){
  case WM_COMMAND:
    if(LOWORD(wp) == 1)
      DestroyWindow(w);
    return 0;
  case WM_PAINT: {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(w, &ps);
    // Information icon: a blue disc with an "i".
    for(int y = 0; y < 28; y++)
      for(int x = 0; x < 28; x++){
        int dx = x - 14, dy = y - 14;
        if(dx * dx + dy * dy <= 13 * 13)
          SetPixel(dc, 14 + x, 14 + y, RGB(0, 0, 200));
      }
    SetTextColor(dc, CLR_HILIGHT);
    TextOut(dc, 24, 20, "i", 1);
    SetTextColor(dc, CLR_TEXT);
    const char *s = (const char *)w->userdata;
    int y = 16;
    while(s && *s){
      int n = 0;
      while(s[n] && s[n] != '\n')
        n++;
      TextOut(dc, 56, y, s, n);
      y += FONT_H + 2;
      s += n;
      if(*s == '\n')
        s++;
    }
    EndPaint(w, &ps);
    return 0;
  }
  case WM_DESTROY:
    free((void *)w->userdata);
    return 0;
  }
  return DefWindowProc(w, msg, wp, lp);
}

int
MessageBox(HWND owner, const char *text, const char *caption, uint type)
{
  (void)owner; (void)type;
  int lines = 1, cols = 0, c = 0;
  for(const char *s = text; *s; s++){
    if(*s == '\n'){ lines++; c = 0; }
    else if(++c > cols) cols = c;
  }
  int w = 56 + cols * FONT_W + 24, h = 16 + lines * (FONT_H + 2) + 50;
  if(w < 200) w = 200;
  int ch = CAPTION_H + 2 * FRAME;
  HWND box = CreateWindow("#msgbox", caption, WS_CAPTION | WS_VISIBLE |
                          WS_NOTASKBAR, (SW - w) / 2, (SH - h - ch) / 2,
                          w, h + ch, 0, 0, 0);
  if(!box)
    return 0;
  box->userdata = (long)strdup_(text);
  RECT cr;
  GetClientRect(box, &cr);
  HWND ok = CreateWindow("BUTTON", "OK", WS_CHILD | WS_VISIBLE,
                         (cr.right - 75) / 2, cr.bottom - 34, 75, 24, box,
                         1, 0);
  SetFocus(ok);
  return 1;
}

// ---------------------------------------------------------------------------
// Start-up.

int
WinInit(void)
{
  struct fb_var_screeninfo var;
  struct fb_fix_screeninfo fix;
  int fd = open("/dev/fb0", O_RDWR);

  if(fd < 0){
    printf("win: cannot open /dev/fb0\n");
    return -1;
  }
  if(ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0 ||
     ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0 || var.bits_per_pixel != 32){
    printf("win: /dev/fb0 is not a 32 bpp frame buffer\n");
    close(fd);
    return -1;
  }
  SW = var.xres;
  SH = var.yres;
  stride = fix.line_length / 4;
  roff = var.red.offset;
  goff = var.green.offset;
  boff = var.blue.offset;
  if(roff == boff){ roff = 16; goff = 8; boff = 0; }
  fb = mmap(0, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if(fb == MAP_FAILED || (bb = malloc(SW * SH * 4)) == 0){
    printf("win: cannot map the frame buffer\n");
    return -1;
  }

  static const struct { const char *name; WNDPROC proc; COLORREF bg; } builtin[] = {
    { "BUTTON", ButtonProc, (COLORREF)-1 },
    { "STATIC", StaticProc, CLR_FACE },
    { "EDIT", EditProc, (COLORREF)-1 },
    { "LISTBOX", ListBoxProc, CLR_WINDOW },
    { "#menu", MenuProc, (COLORREF)-1 },
    { "#msgbox", MsgBoxProc, CLR_FACE },
  };
  for(uint i = 0; i < sizeof(builtin) / sizeof(builtin[0]); i++){
    WNDCLASS wc = { builtin[i].name, builtin[i].proc, builtin[i].bg, 0 };
    RegisterClass(&wc);
  }
  return 0;
}

void
WinExit(void)
{
  // Destroying our windows lets the kernel repaint what they covered; the
  // kernel would do the same when the process exits.
  for(int i = 0; i < MAXWIN; i++)
    if(windows[i].used && !windows[i].parent)
      DestroyWindow(&windows[i]);
}
