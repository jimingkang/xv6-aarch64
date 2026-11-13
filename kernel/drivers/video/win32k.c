// win32k: a miniature of the Windows NT kernel-mode window manager.
//
// Like win32k.sys (and ReactOS's win32k), the kernel owns the things that
// must be shared and trusted across processes:
//
//   - top-level window objects (HWND = kernel handle), their owner process,
//     position, title and Z order, and which one is active;
//   - one message queue per process (thread group); GetMessage sleeps here;
//   - input: USB keyboard/mouse events from the input core are hit-tested
//     and posted to the queue of the window under the cursor (or the active
//     window for keys); the mouse cursor is drawn by the kernel;
//   - non-client behaviour: dragging by the caption, the minimize and close
//     buttons, a click outside a popup menu cancelling it;
//   - painting: each window has an update rectangle; WM_PAINT is synthesized
//     when the queue is empty;
//   - GDI: every window has a kernel backing surface.  User mode sends
//     batches of drawing commands (W32_GDI, like NT's GDI batching) that are
//     rasterized here, with the font in the kernel;
//   - composition: the kernel alone writes /dev/fb0, copying the surfaces
//     onto the screen in Z order.  Moving or uncovering a window is pure
//     composition -- applications are not asked to repaint -- a small
//     version of what the Desktop Window Manager does on top of win32k.
//
// Child controls stay in each process's user32 library.  When a process
// exits, win32k_exit() destroys its windows.

#include <kernel/types.h>
#include <kernel/param.h>
#include "memlayout.h"
#include "aarch64.h"
#include <kernel/spinlock.h>
#include <kernel/sleeplock.h>
#include <kernel/fs.h>
#include <kernel/proc.h>
#include <kernel/file.h>
#include <kernel/defs.h>
#include <kernel/workqueue.h>
#include <kernel/input.h>
#include <kernel/module.h>
#include <kernel/win32k.h>
#include <uapi/win32k.h>
#include "user/font8x16.h"

#define MAXW  64
#define MAXQ  16
#define QLEN  256
#define RAWQ  512
#define CUR_W 12
#define CUR_H 19

struct kwnd {
  int used;
  uint gen;
  int owner;                    // tgid
  uint32 style;
  struct w32_rect rc;           // screen
  char title[64];
  struct w32_rect upd;          // pending repaint, window coordinates
  int minimized;
  uint seq;                     // creation order (taskbar)
  uint32 *surf;                 // backing surface, sw x sh, fb pixel format
  int sw, sh;
  int painted;                  // the surface holds a complete picture
  struct w32_rect painting;     // update area handed out by BeginPaint
};

struct kq {
  int used;
  int tgid;
  struct w32_msg ring[QLEN];
  uint head, tail;
  struct delayed_work timer;    // ends a GETMSG timeout
};

static struct spinlock lk;
static struct kwnd wins[MAXW];
static int z[MAXW], nz;         // bottom to top
static struct kq queues[MAXQ];
static int nwin;
static int active = -1, shellw = -1, capture = -1, dragw = -1, ncw = -1;
static int ncbtn, drag_dx, drag_dy;
static uint seqno;

static struct fb_kinfo fbk;
static int fb_ok;
static int mx, my, mouse_seen, pdx, pdy;
static int cursor_drawn, cursor_hide, csx, csy;
static uint32 cursor_save[CUR_W * CUR_H];
static int shift, ctrl, caps;
static uint64 last_click_us;
static int last_click_w = -1, last_click_x, last_click_y;

static struct spinlock rawlk;
static struct { uint16 type, code; int value; } raw[RAWQ];
static uint rawh, rawt;
static struct workqueue w32wq;
static struct work_struct input_work;

static const char *cursor_img[CUR_H] = {
  "X           ", "XX          ", "X.X         ", "X..X        ",
  "X...X       ", "X....X      ", "X.....X     ", "X......X    ",
  "X.......X   ", "X........X  ", "X.........X ", "X......XXXXX",
  "X...X..X    ", "X..XX..X    ", "X.X  X..X   ", "XX   X..X   ",
  "X     X..X  ", "      X..X  ", "       XX   ",
};

// ---------------------------------------------------------------------------
// Small helpers.

static uint64
now_us(void)
{
  return r_cntvct_el0() / (r_cntfrq_el0() / 1000000);
}

static int
rempty(const struct w32_rect *r)
{
  return r->right <= r->left || r->bottom <= r->top;
}

static struct w32_rect
risect(struct w32_rect a, struct w32_rect b)
{
  struct w32_rect r;
  r.left = a.left > b.left ? a.left : b.left;
  r.top = a.top > b.top ? a.top : b.top;
  r.right = a.right < b.right ? a.right : b.right;
  r.bottom = a.bottom < b.bottom ? a.bottom : b.bottom;
  return r;
}

static void
runion(struct w32_rect *d, struct w32_rect r)
{
  if(rempty(&r))
    return;
  if(rempty(d)){
    *d = r;
    return;
  }
  if(r.left < d->left) d->left = r.left;
  if(r.top < d->top) d->top = r.top;
  if(r.right > d->right) d->right = r.right;
  if(r.bottom > d->bottom) d->bottom = r.bottom;
}

static int
pt_in(struct w32_rect r, int x, int y)
{
  return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

static uint32
hwnd_of(int i)
{
  return i < 0 ? 0 : (wins[i].gen << 8) | (uint32)(i + 1);
}

static int
index_of(uint32 h)
{
  int i = (int)(h & 0xff) - 1;
  if(i < 0 || i >= MAXW || !wins[i].used || wins[i].gen != (h >> 8))
    return -1;
  return i;
}

static int
shown(int i)
{
  return wins[i].used && (wins[i].style & W32S_VISIBLE) && !wins[i].minimized;
}

static uint32
desktop_pixel(void)
{
  int r = 58, g = 110, b = 165;
  return fbk.rgb ? (uint32)(r | g << 8 | b << 16) :
                   (uint32)(b | g << 8 | r << 16);
}

// ---------------------------------------------------------------------------
// Queues.

static struct kq *
queue_of(int tgid, int create)
{
  struct kq *free = 0;
  for(int i = 0; i < MAXQ; i++){
    if(queues[i].used && queues[i].tgid == tgid)
      return &queues[i];
    if(!queues[i].used && !free)
      free = &queues[i];
  }
  if(!create || !free)
    return 0;
  free->used = 1;
  free->tgid = tgid;
  free->head = free->tail = 0;
  return free;
}

// Caller holds lk.  Consecutive mouse moves to one window are merged, as
// Windows does, so a slow application does not drown in motion events.
static void
post_to(int tgid, uint32 hwnd, uint32 msg, uint64 wp, long lp)
{
  struct kq *q = queue_of(tgid, 1);
  if(!q)
    return;
  if(q->head != q->tail){
    struct w32_msg *last = &q->ring[(q->tail - 1) % QLEN];
    if(last->hwnd == hwnd && last->message == msg &&
       (msg == W32M_MOUSEMOVE || msg == W32M_SHELLHOOK)){
      last->wparam = wp;
      last->lparam = lp;
      return;
    }
  }
  if(q->tail - q->head >= QLEN)
    return;                     // full: drop, like a lost input event
  struct w32_msg *m = &q->ring[q->tail++ % QLEN];
  m->hwnd = hwnd;
  m->message = msg;
  m->wparam = wp;
  m->lparam = lp;
  wakeup(q);
}

static void
post_win(int i, uint32 msg, uint64 wp, long lp)
{
  if(i >= 0 && wins[i].used)
    post_to(wins[i].owner, hwnd_of(i), msg, wp, lp);
}

static void
shell_notify(void)
{
  if(shellw >= 0 && wins[shellw].used)
    post_win(shellw, W32M_SHELLHOOK, 0, 0);
}

static void
timer_fn(struct work_struct *w)
{
  struct kq *q = (struct kq *)((char *)w -
                  __builtin_offsetof(struct kq, timer.work));
  acquire(&lk);
  wakeup(q);
  release(&lk);
}

// ---------------------------------------------------------------------------
// The cursor: drawn by the kernel over whatever is on the screen, with the
// pixels underneath saved.  It is lifted while any window paints.

static void
cursor_erase(void)
{
  if(!cursor_drawn)
    return;
  for(int r = 0; r < CUR_H; r++)
    for(int c = 0; c < CUR_W; c++){
      int x = csx + c, y = csy + r;
      if(x < fbk.width && y < fbk.height && cursor_img[r][c] != ' ')
        fbk.pixels[y * fbk.stride + x] = cursor_save[r * CUR_W + c];
    }
  cursor_drawn = 0;
}

static void
cursor_draw(void)
{
  if(cursor_drawn || cursor_hide || !mouse_seen || !fb_ok || nwin == 0)
    return;
  csx = mx;
  csy = my;
  for(int r = 0; r < CUR_H; r++)
    for(int c = 0; c < CUR_W; c++){
      int x = csx + c, y = csy + r;
      char p = cursor_img[r][c];
      if(x >= fbk.width || y >= fbk.height || p == ' ')
        continue;
      uint32 *px = &fbk.pixels[y * fbk.stride + x];
      cursor_save[r * CUR_W + c] = *px;
      *px = p == 'X' ? 0x000000 : 0xffffff;
    }
  cursor_drawn = 1;
}

// ---------------------------------------------------------------------------
// Z order, damage, activation.

static int
window_at(int x, int y)
{
  for(int k = nz - 1; k >= 0; k--){
    int i = z[k];
    if(shown(i) && pt_in(wins[i].rc, x, y))
      return i;
  }
  return -1;
}

static void
z_remove(int i)
{
  int j = 0;
  for(int k = 0; k < nz; k++)
    if(z[k] != i)
      z[j++] = z[k];
  nz = j;
}

// Mark the part of screen rectangle s that lies on window i for repaint.
static void
invalidate_screen_on(int i, struct w32_rect s)
{
  struct w32_rect r = risect(s, wins[i].rc);
  if(rempty(&r))
    return;
  r.left -= wins[i].rc.left;
  r.right -= wins[i].rc.left;
  r.top -= wins[i].rc.top;
  r.bottom -= wins[i].rc.top;
  runion(&wins[i].upd, r);
  struct kq *q = queue_of(wins[i].owner, 0);
  if(q)
    wakeup(q);
}

// Recompose screen rectangle s from the backing surfaces: the background,
// then every shown window bottom to top (painter's algorithm).  This is the
// only code that writes window pixels to the frame buffer.
static void
compose(struct w32_rect s)
{
  if(!fb_ok)
    return;
  struct w32_rect scr = { 0, 0, fbk.width, fbk.height };
  s = risect(s, scr);
  if(rempty(&s))
    return;
  int was = cursor_drawn;
  cursor_erase();
  uint32 bg = nwin ? desktop_pixel() : 0;
  for(int y = s.top; y < s.bottom; y++){
    uint32 *row = fbk.pixels + y * fbk.stride;
    for(int x = s.left; x < s.right; x++)
      row[x] = bg;
  }
  for(int k = 0; k < nz; k++){
    struct kwnd *w = &wins[z[k]];
    if(!shown(z[k]) || !w->painted || !w->surf)
      continue;
    struct w32_rect r = risect(s, w->rc);
    for(int y = r.top; y < r.bottom; y++)
      memmove(fbk.pixels + y * fbk.stride + r.left,
              w->surf + (y - w->rc.top) * w->sw + (r.left - w->rc.left),
              (r.right - r.left) * 4);
  }
  if(was || mouse_seen)
    cursor_draw();
}

static void
damage_screen(struct w32_rect s)
{
  compose(s);
}

static void
raise_win(int i)
{
  z_remove(i);
  int pos = nz;
  if(wins[i].style & W32S_BOTTOM){
    pos = 0;
  } else if(!(wins[i].style & W32S_TOPMOST)){
    while(pos > 0 && (wins[z[pos - 1]].style & W32S_TOPMOST))
      pos--;
  }
  for(int k = nz; k > pos; k--)
    z[k] = z[k - 1];
  z[pos] = i;
  nz++;
  compose(wins[i].rc);          // its surface is now on top
}

static void
activate(int i)
{
  int old = active;
  if(i >= 0 && !(wins[i].style & W32S_BOTTOM))
    raise_win(i);
  if(old == i)
    return;
  active = i;
  // Both captions change colour: the owners repaint their frames.
  if(old >= 0 && wins[old].used){
    invalidate_screen_on(old, wins[old].rc);
    post_win(old, W32M_ACTIVATE, 0, 0);
  }
  if(i >= 0){
    invalidate_screen_on(i, wins[i].rc);
    post_win(i, W32M_ACTIVATE, 1, 0);
  }
  shell_notify();
}

// After the active window goes away, activate the topmost captioned one.
static void
activate_next(void)
{
  for(int k = nz - 1; k >= 0; k--){
    int i = z[k];
    if(shown(i) && (wins[i].style & W32S_CAPTION)){
      activate(i);
      return;
    }
  }
  active = -1;
  shell_notify();
}

static void
destroy_locked(int i)
{
  struct w32_rect r = wins[i].rc;
  int was_shown = shown(i);
  uint32 h = hwnd_of(i);

  wins[i].used = 0;
  wins[i].gen++;
  nwin--;
  z_remove(i);
  // Drop messages still queued for it.
  for(int qi = 0; qi < MAXQ; qi++)
    if(queues[qi].used)
      for(uint k = queues[qi].head; k != queues[qi].tail; k++)
        if(queues[qi].ring[k % QLEN].hwnd == h)
          queues[qi].ring[k % QLEN].hwnd = 0;
  if(capture == i) capture = -1;
  if(dragw == i) dragw = -1;
  if(ncw == i) ncw = -1;
  if(shellw == i) shellw = -1;
  if(last_click_w == i) last_click_w = -1;
  if(was_shown)
    damage_screen(r);
  kfree(wins[i].surf);          // after composing: nothing shows it now
  wins[i].surf = 0;
  if(active == i){
    active = -1;
    activate_next();
  }
  if(nwin == 0){
    // Session over: the screen and the keyboard go back to the console.
    cursor_erase();
    mouse_seen = 0;
    cursor_hide = 0;
  } else {
    cursor_draw();
  }
  shell_notify();
}

static int
menu_window(void)
{
  for(int k = nz - 1; k >= 0; k--)
    if(shown(z[k]) && (wins[z[k]].style & W32S_MENU))
      return z[k];
  return -1;
}

// ---------------------------------------------------------------------------
// Input: called from the input core for every event (device lock held), so
// it only queues; the work below does the routing in process context.

int
win32k_owns_input(void)
{
  return nwin > 0;
}

void
win32k_input_event(uint16 type, uint16 code, int value)
{
  if(nwin == 0)
    return;
  acquire(&rawlk);
  if(rawt - rawh < RAWQ){
    raw[rawt % RAWQ].type = type;
    raw[rawt % RAWQ].code = code;
    raw[rawt % RAWQ].value = value;
    rawt++;
  }
  release(&rawlk);
  queue_work(&w32wq, &input_work);
}

static long
wpos(int i, int x, int y)
{
  int lx = x - wins[i].rc.left, ly = y - wins[i].rc.top;
  return (long)(((uint32)lx & 0xffff) | (((uint32)ly & 0xffff) << 16));
}

static void
caption_buttons(int i, struct w32_rect *mn, struct w32_rect *cl)
{
  int w = wins[i].rc.right - wins[i].rc.left;
  cl->left = wins[i].rc.left + w - W32_FRAME - 2 - W32_BTN_W;
  cl->top = wins[i].rc.top + W32_FRAME + 2;
  cl->right = cl->left + W32_BTN_W;
  cl->bottom = cl->top + W32_BTN_H;
  *mn = *cl;
  mn->left -= 18;
  mn->right -= 18;
}

static void
mouse_move(int dx, int dy)
{
  if(!fb_ok)
    return;
  if(!mouse_seen){
    mouse_seen = 1;
    mx = fbk.width / 2;
    my = fbk.height / 2;
  }
  mx += dx;
  my += dy;
  if(mx < 0) mx = 0;
  if(my < 0) my = 0;
  if(mx >= fbk.width) mx = fbk.width - 1;
  if(my >= fbk.height) my = fbk.height - 1;
  cursor_erase();
  if(dragw >= 0){
    struct kwnd *w = &wins[dragw];
    int ww = w->rc.right - w->rc.left, hh = w->rc.bottom - w->rc.top;
    int nx = mx - drag_dx, ny = my - drag_dy;
    if(ny < 0) ny = 0;
    if(ny > fbk.height - 30) ny = fbk.height - 30;
    if(nx > fbk.width - 40) nx = fbk.width - 40;
    if(nx + ww < 40) nx = 40 - ww;
    if(nx != w->rc.left || ny != w->rc.top){
      struct w32_rect old = w->rc;
      w->rc.left = nx; w->rc.top = ny;
      w->rc.right = nx + ww; w->rc.bottom = ny + hh;
      // Pure composition: the window and what it uncovers come from
      // their backing surfaces, no WM_PAINT round trip.
      compose(old);
      compose(w->rc);
    }
  } else {
    int t = capture >= 0 ? capture : window_at(mx, my);
    if(t >= 0)
      post_win(t, W32M_MOUSEMOVE, 0, wpos(t, mx, my));
  }
  cursor_draw();
}

static void
button(int left, int down)
{
  if(!mouse_seen)
    mouse_move(0, 0);
  if(!left){
    if(down){
      int t = window_at(mx, my);
      if(t >= 0)
        post_win(t, W32M_RBUTTONDOWN, 0, wpos(t, mx, my));
    }
    return;
  }
  if(!down){
    if(dragw >= 0){
      dragw = -1;
      return;
    }
    if(ncw >= 0){
      int i = ncw, which = ncbtn;
      struct w32_rect mn, cl;
      ncw = -1;
      ncbtn = 0;
      invalidate_screen_on(i, wins[i].rc);
      caption_buttons(i, &mn, &cl);
      if(which == 1 && pt_in(mn, mx, my)){
        wins[i].minimized = 1;
        damage_screen(wins[i].rc);
        if(active == i){
          active = -1;
          activate_next();
        }
        shell_notify();
      } else if(which == 2 && pt_in(cl, mx, my)){
        post_win(i, W32M_CLOSE, 0, 0);
      }
      return;
    }
    if(capture >= 0){
      int c = capture;
      capture = -1;
      post_win(c, W32M_LBUTTONUP, 0, wpos(c, mx, my));
    }
    return;
  }

  // A click outside an open popup menu only cancels the menu.
  int menu = menu_window();
  if(menu >= 0 && !pt_in(wins[menu].rc, mx, my)){
    post_win(menu, W32M_CANCELMODE, 0, 0);
    return;
  }
  int i = window_at(mx, my);
  if(i < 0)
    return;
  struct kwnd *w = &wins[i];
  if((w->style & W32S_CAPTION) || !(w->style & (W32S_TOPMOST | W32S_MENU)))
    activate(i);
  if(w->style & W32S_CAPTION){
    int lx = mx - w->rc.left, ly = my - w->rc.top;
    int in_client = lx >= W32_FRAME && ly >= W32_FRAME + W32_CAPTION_H &&
                    mx < w->rc.right - W32_FRAME && my < w->rc.bottom - W32_FRAME;
    if(!in_client){
      struct w32_rect mn, cl;
      caption_buttons(i, &mn, &cl);
      if(pt_in(mn, mx, my) || pt_in(cl, mx, my)){
        ncw = i;
        ncbtn = pt_in(mn, mx, my) ? 1 : 2;
        invalidate_screen_on(i, w->rc);
      } else if(ly < W32_FRAME + W32_CAPTION_H){
        dragw = i;
        drag_dx = lx;
        drag_dy = ly;
      }
      return;
    }
  }
  uint64 t = now_us();
  int dbl = i == last_click_w && t - last_click_us < 450000 &&
            mx - last_click_x < 5 && last_click_x - mx < 5 &&
            my - last_click_y < 5 && last_click_y - my < 5;
  last_click_us = dbl ? 0 : t;
  last_click_w = i;
  last_click_x = mx;
  last_click_y = my;
  capture = i;
  post_win(i, dbl ? W32M_LBUTTONDBLCLK : W32M_LBUTTONDOWN, 0, wpos(i, mx, my));
}

// Linux key code -> character (the keyboard layout lives in the kernel,
// as in win32k).
static const char keymap[] =
  "\0\0331234567890-=\b\tqwertyuiop[]\r\0asdfghjkl;'`\0\\zxcvbnm,./";
static const char shiftmap[] =
  "\0\033!@#$%^&*()_+\b\tQWERTYUIOP{}\r\0ASDFGHJKL:\"~\0|ZXCVBNM<>?";

static void
key(int code, int down)
{
  if(code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT){ shift = down; return; }
  if(code == KEY_LEFTCTRL || code == KEY_RIGHTCTRL){ ctrl = down; return; }
  if(!down)
    return;
  if(code == KEY_CAPSLOCK){ caps ^= 1; return; }
  int t = menu_window();
  if(t < 0)
    t = active;
  if(t < 0 || !wins[t].used)
    return;
  int vk = 0, ch = 0;
  switch(code){
  case KEY_UP: vk = 0x26; break;
  case KEY_DOWN: vk = 0x28; break;
  case KEY_LEFT: vk = 0x25; break;
  case KEY_RIGHT: vk = 0x27; break;
  case KEY_SPACE: vk = ' '; ch = ' '; break;
  default:
    if(code > 0 && code < (int)sizeof(keymap) - 1 && keymap[code]){
      char c = keymap[code];
      int up = shift;
      if(c >= 'a' && c <= 'z')
        up ^= caps;
      if(up)
        c = shiftmap[code];
      vk = (c >= 'a' && c <= 'z') ? c - 32 : c;
      if(ctrl && (c | 0x20) >= 'a' && (c | 0x20) <= 'z')
        c = (c | 0x20) - 'a' + 1;
      ch = c;
    }
  }
  if(vk)
    post_win(t, W32M_KEYDOWN, vk, 0);
  if(ch)
    post_win(t, W32M_CHAR, (uchar)ch, 0);
}

static void
input_work_fn(struct work_struct *work)
{
  (void)work;
  for(;;){
    uint16 type, code;
    int value;
    acquire(&rawlk);
    if(rawh == rawt){
      release(&rawlk);
      return;
    }
    type = raw[rawh % RAWQ].type;
    code = raw[rawh % RAWQ].code;
    value = raw[rawh % RAWQ].value;
    rawh++;
    release(&rawlk);

    acquire(&lk);
    if(nwin > 0){
      if(type == EV_REL){
        if(code == REL_X)
          pdx += value;
        else if(code == REL_Y)
          pdy += value;
        else if(code == REL_WHEEL && mouse_seen){
          int t = window_at(mx, my);
          if(t >= 0)
            post_win(t, W32M_MOUSEWHEEL, (uint64)(long)value, wpos(t, mx, my));
        }
      } else if(type == EV_SYN && code == SYN_REPORT){
        if(pdx || pdy)
          mouse_move(pdx, pdy);
        pdx = pdy = 0;
      } else if(type == EV_KEY){
        if(code == BTN_LEFT || code == BTN_RIGHT){
          if(pdx || pdy)
            mouse_move(pdx, pdy);
          pdx = pdy = 0;
          button(code == BTN_LEFT, value);
        } else if(code < BTN_LEFT){
          key(code, value);
        }
      }
    }
    release(&lk);
  }
}

// ---------------------------------------------------------------------------
// Process exit: its windows and queue go away.

void
win32k_exit(int tgid)
{
  acquire(&lk);
  for(int i = 0; i < MAXW; i++)
    if(wins[i].used && wins[i].owner == tgid)
      destroy_locked(i);
  struct kq *q = queue_of(tgid, 0);
  if(q)
    q->used = 0;
  release(&lk);
  if(q)
    cancel_delayed_work_sync(&q->timer);
}

// ---------------------------------------------------------------------------
// The system call.

static int
sys_create(uint64 uarg)
{
  struct w32_create c;
  struct proc *p = myproc();
  if(copyin(p->vm->pagetable, (char *)&c, uarg, sizeof(c)) < 0)
    return 0;
  c.title[sizeof(c.title) - 1] = 0;
  if(c.w <= 0 || c.h <= 0 || c.w > 4096 || c.h > 4096)
    return 0;
  if(!fb_ok){
    if(fb_kernel_info(&fbk) < 0)
      return 0;
    fb_ok = 1;
  }
  acquire(&lk);
  int i;
  for(i = 0; i < MAXW && wins[i].used; i++)
    ;
  if(i == MAXW || !queue_of(p->tgid, 1)){
    release(&lk);
    return 0;
  }
  uint32 *surf = kmalloc((uint64)c.w * c.h * 4);   // zeroed
  if(!surf){
    release(&lk);
    return 0;
  }
  struct kwnd *w = &wins[i];
  uint gen = w->gen;
  memset(w, 0, sizeof(*w));
  w->surf = surf;
  w->sw = c.w;
  w->sh = c.h;
  w->upd.right = c.w;           // everything needs a first paint
  w->upd.bottom = c.h;
  w->gen = gen;
  w->used = 1;
  w->owner = p->tgid;
  w->style = c.style;
  w->rc.left = c.x;
  w->rc.top = c.y;
  w->rc.right = c.x + c.w;
  w->rc.bottom = c.y + c.h;
  safestrcpy(w->title, c.title, sizeof(w->title));
  w->seq = ++seqno;
  if(nwin++ == 0){
    struct w32_rect all = { 0, 0, fbk.width, fbk.height };
    cursor_hide = 0;
    compose(all);               // the session starts on a clean desktop
  }
  z[nz++] = i;
  raise_win(i);
  if((w->style & W32S_VISIBLE) && (w->style & W32S_CAPTION))
    activate(i);
  else if((w->style & W32S_BOTTOM) && active < 0)
    active = i;
  shell_notify();
  uint32 h = hwnd_of(i);
  release(&lk);
  return h;
}

static long
sys_getmsg(uint64 uptr, int timeout_ms)
{
  struct proc *p = myproc();
  struct w32_msg m;
  uint64 deadline = 0;
  int armed = 0;

  if(timeout_ms > 0)
    deadline = workqueue_now() + (timeout_ms + 9) / 10;
  acquire(&lk);
  struct kq *q = queue_of(p->tgid, 1);
  if(!q){
    release(&lk);
    return -1;
  }
  for(;;){
    while(q->head != q->tail){
      m = q->ring[q->head++ % QLEN];
      if(m.hwnd == 0 && m.message != W32M_QUIT)
        continue;               // its window was destroyed
      release(&lk);
      if(copyout(p->vm->pagetable, uptr, (char *)&m, sizeof(m)) < 0)
        return -1;
      return m.message == W32M_QUIT ? 0 : 1;
    }
    // Queue empty: synthesize WM_PAINT for a window that needs it.
    for(int k = nz - 1; k >= 0; k--){
      int i = z[k];
      if(shown(i) && wins[i].owner == p->tgid && !rempty(&wins[i].upd)){
        m.hwnd = hwnd_of(i);
        m.message = W32M_PAINT;
        m.wparam = 0;
        m.lparam = 0;
        release(&lk);
        if(copyout(p->vm->pagetable, uptr, (char *)&m, sizeof(m)) < 0)
          return -1;
        return 1;
      }
    }
    if(timeout_ms == 0 || p->killed ||
       (timeout_ms > 0 && workqueue_now() >= deadline)){
      release(&lk);
      return p->killed ? -1 : 2;            // 2: timed out, no message
    }
    if(timeout_ms > 0 && !armed){
      uint64 now = workqueue_now();
      mod_delayed_work(&w32wq, &q->timer, deadline > now ? deadline - now : 1);
      armed = 1;
    }
    sleep(q, &lk);
    if(timeout_ms > 0 && workqueue_now() < deadline)
      armed = 0;                // woken early; re-arm for the remainder
  }
}

static long
sys_beginpaint(int i, uint64 uptr)
{
  struct w32_paint ps;
  memset(&ps, 0, sizeof(ps));
  ps.rc = wins[i].rc;
  ps.update = wins[i].upd;
  ps.active = i == active;
  runion(&wins[i].painting, wins[i].upd);
  wins[i].upd.left = wins[i].upd.right = 0;
  release(&lk);
  if(copyout(myproc()->vm->pagetable, uptr, (char *)&ps, sizeof(ps)) < 0)
    return -1;
  return 0;
}

static uint32
gdi_pixel(uint32 colorref)
{
  uint32 r = colorref & 0xff, g = (colorref >> 8) & 0xff,
         b = (colorref >> 16) & 0xff;
  return fbk.rgb ? (r | g << 8 | b << 16) : (b | g << 8 | r << 16);
}

// Rasterize one batch of drawing commands into window i's surface.  Runs
// without lk: only the owner draws, and only the owner (or its exit) frees
// the surface; the compositor may read a half-drawn surface, which merely
// shows up once more after EndPaint recomposes.
static long
sys_gdi(uint32 h, uint64 ubuf, int len)
{
  struct proc *p = myproc();
  char *buf;
  if(len <= 0)
    return 0;
  if(len > 32768)
    return -1;
  if((buf = kmalloc(len)) == 0)
    return -1;
  if(copyin(p->vm->pagetable, buf, ubuf, len) < 0){
    kfree(buf);
    return -1;
  }
  acquire(&lk);
  int i = index_of(h);
  if(i < 0 || wins[i].owner != p->tgid || !wins[i].surf){
    release(&lk);
    kfree(buf);
    return -1;
  }
  uint32 *surf = wins[i].surf;
  int sw = wins[i].sw, sh = wins[i].sh;
  release(&lk);

  int off = 0, n = 0;
  while(off + (int)sizeof(struct w32_gdicmd) <= len){
    struct w32_gdicmd *c = (struct w32_gdicmd *)(buf + off);
    int textlen = c->len;
    int next = off + sizeof(*c) + ((textlen + 3) & ~3);
    if(next > len)
      break;
    // Clip to the command's clip box and the surface.
    int cl = c->clip_left > 0 ? c->clip_left : 0;
    int ct = c->clip_top > 0 ? c->clip_top : 0;
    int cr = c->clip_right < sw ? c->clip_right : sw;
    int cb = c->clip_bottom < sh ? c->clip_bottom : sh;
    uint32 px = gdi_pixel(c->color);
    if(c->op == GDI_FILL){
      int x0 = c->x0 > cl ? c->x0 : cl, y0 = c->y0 > ct ? c->y0 : ct;
      int x1 = c->x1 < cr ? c->x1 : cr, y1 = c->y1 < cb ? c->y1 : cb;
      for(int y = y0; y < y1; y++){
        uint32 *row = surf + y * sw;
        for(int x = x0; x < x1; x++)
          row[x] = px;
      }
    } else if(c->op == GDI_TEXT){
      const char *t = (const char *)(c + 1);
      for(int k = 0; k < textlen; k++){
        int ch = (uchar)t[k], gx = c->x0 + k * FONT_W;
        if(ch < FONT_FIRST || ch > FONT_LAST || gx >= cr)
          continue;
        const uchar *g = font8x16[ch - FONT_FIRST];
        for(int r = 0; r < FONT_H; r++){
          int y = c->y0 + r;
          if(y < ct || y >= cb)
            continue;
          uint bits = g[r];
          if(c->y1)
            bits |= bits >> 1;          // bold
          for(int b = 0; b < FONT_W; b++){
            int x = gx + b;
            if(x >= cl && x < cr && (bits & (0x80 >> b)))
              surf[y * sw + x] = px;
          }
        }
      }
    }
    off = next;
    n++;
  }
  kfree(buf);
  return n;
}

long
sys_win32k(void)
{
  int op;
  uint64 a1, a2, a3;
  struct proc *p = myproc();

  if(argint(0, &op) < 0 || argaddr(1, &a1) < 0 || argaddr(2, &a2) < 0 ||
     argaddr(3, &a3) < 0)
    return -1;

  switch(op){
  case W32_CREATE:
    return sys_create(a1);
  case W32_GETMSG:
    return sys_getmsg(a1, (int)a2);
  case W32_GDI:
    return sys_gdi((uint32)a1, a2, (int)a3);
  case W32_SCREEN:
    if(!fb_ok){
      if(fb_kernel_info(&fbk) < 0)
        return -1;
      fb_ok = 1;
    }
    return fbk.width | (fbk.height << 16);
  case W32_POST: {
    struct w32_msg m;
    if(copyin(p->vm->pagetable, (char *)&m, a1, sizeof(m)) < 0)
      return -1;
    acquire(&lk);
    if(m.hwnd == 0)
      post_to(p->tgid, 0, m.message, m.wparam, m.lparam);
    else {
      int i = index_of(m.hwnd);
      if(i >= 0)
        post_to(wins[i].owner, m.hwnd, m.message, m.wparam, m.lparam);
    }
    release(&lk);
    return 1;
  }
  case W32_QUIT:
    acquire(&lk);
    post_to(p->tgid, 0, W32M_QUIT, a1, 0);
    release(&lk);
    return 1;
  case W32_GETACTIVE: {
    acquire(&lk);
    uint32 h = active >= 0 && wins[active].used ? hwnd_of(active) : 0;
    release(&lk);
    return h;
  }
  case W32_ENUM: {
    uint32 out[MAXW];
    int n = 0, max = (int)a2;
    acquire(&lk);
    for(uint s = 1; s <= seqno && n < MAXW; s++)
      for(int i = 0; i < MAXW; i++)
        if(wins[i].used && wins[i].seq == s &&
           (a3 || ((wins[i].style & W32S_CAPTION) &&
                   (wins[i].style & W32S_VISIBLE) &&
                   !(wins[i].style & W32S_NOTASKBAR))))
          out[n++] = hwnd_of(i);
    release(&lk);
    if(n > max)
      n = max;
    if(n > 0 && copyout(p->vm->pagetable, a1, (char *)out, n * sizeof(uint32)) < 0)
      return -1;
    return n;
  }
  }

  // The rest name a window.
  acquire(&lk);
  int i = index_of((uint32)a1);
  if(i < 0){
    release(&lk);
    return -1;
  }
  struct kwnd *w = &wins[i];
  int mine = w->owner == p->tgid;
  switch(op){
  case W32_DESTROY:
    if(!mine)
      break;
    destroy_locked(i);
    release(&lk);
    return 0;
  case W32_SHOW:
    if(a2){
      w->minimized = 0;
      w->style |= W32S_VISIBLE;
      if(w->style & W32S_CAPTION)
        activate(i);
      else
        raise_win(i);
    } else if(!w->minimized){
      if(w->style & W32S_CAPTION)
        w->minimized = 1;
      else
        w->style &= ~W32S_VISIBLE;
      damage_screen(w->rc);
      if(active == i){
        active = -1;
        activate_next();
      }
    }
    shell_notify();
    release(&lk);
    return 0;
  case W32_ACTIVATE:
    if(w->minimized){
      w->minimized = 0;
      invalidate_screen_on(i, w->rc);
    }
    activate(i);
    release(&lk);
    return 0;
  case W32_SETSHELL:
    if(!mine)
      break;
    shellw = i;
    release(&lk);
    return 0;
  case W32_ENDPAINT: {
    if(!mine)
      break;
    // Show what was just drawn: recompose the painted area on screen.
    struct w32_rect r = w->painting;
    w->painting.left = w->painting.right = 0;
    if(!w->painted){
      w->painted = 1;
      r.left = r.top = 0;
      r.right = w->sw;
      r.bottom = w->sh;
    }
    r.left += w->rc.left; r.right += w->rc.left;
    r.top += w->rc.top; r.bottom += w->rc.top;
    compose(r);
    release(&lk);
    return 0;
  }
  case W32_BEGINPAINT:
    if(!mine)
      break;
    return sys_beginpaint(i, a2);       // releases lk
  case W32_INVALIDATE: {
    if(!mine)
      break;
    struct w32_rect r;
    if(a2){
      release(&lk);
      if(copyin(p->vm->pagetable, (char *)&r, a2, sizeof(r)) < 0)
        return -1;
      acquire(&lk);
      if((i = index_of((uint32)a1)) < 0)
        break;
      r.left += wins[i].rc.left; r.right += wins[i].rc.left;
      r.top += wins[i].rc.top; r.bottom += wins[i].rc.top;
    } else {
      r = w->rc;
    }
    invalidate_screen_on(i, r);
    release(&lk);
    return 0;
  }
  case W32_MOVE: {
    if(!mine)
      break;
    struct w32_rect r;
    release(&lk);
    if(copyin(p->vm->pagetable, (char *)&r, a2, sizeof(r)) < 0)
      return -1;
    acquire(&lk);
    if((i = index_of((uint32)a1)) < 0 || rempty(&r))
      break;
    struct w32_rect old = wins[i].rc;
    int nw = r.right - r.left, nh = r.bottom - r.top;
    if(nw != wins[i].sw || nh != wins[i].sh){
      uint32 *ns = kmalloc((uint64)nw * nh * 4);
      if(!ns)
        break;
      kfree(wins[i].surf);
      wins[i].surf = ns;
      wins[i].sw = nw;
      wins[i].sh = nh;
      wins[i].painted = 0;
      wins[i].upd.left = wins[i].upd.top = 0;
      wins[i].upd.right = nw;
      wins[i].upd.bottom = nh;
      struct kq *q = queue_of(wins[i].owner, 0);
      if(q)
        wakeup(q);
    }
    wins[i].rc = r;
    compose(old);
    compose(r);
    release(&lk);
    return 0;
  }
  case W32_SETTEXT: {
    if(!mine)
      break;
    char t[64];
    release(&lk);
    if(copyinstr(p->vm->pagetable, t, a2, sizeof(t)) < 0)
      return -1;
    acquire(&lk);
    if((i = index_of((uint32)a1)) < 0)
      break;
    safestrcpy(wins[i].title, t, sizeof(wins[i].title));
    struct w32_rect cap = wins[i].rc;
    cap.bottom = cap.top + W32_FRAME + W32_CAPTION_H;
    invalidate_screen_on(i, cap);
    shell_notify();
    release(&lk);
    return 0;
  }
  case W32_GETTEXT: {
    char t[64];
    int max = (int)a3;
    safestrcpy(t, w->title, sizeof(t));
    release(&lk);
    if(max <= 0)
      return 0;
    if(max > (int)sizeof(t))
      max = sizeof(t);
    t[max - 1] = 0;
    int n = strlen(t);
    if(copyout(p->vm->pagetable, a2, t, n + 1) < 0)
      return -1;
    return n;
  }
  case W32_GETINFO: {
    struct w32_info info;
    info.rc = w->rc;
    info.style = w->style;
    info.owner = w->owner;
    info.minimized = w->minimized;
    info.active = i == active;
    release(&lk);
    if(copyout(p->vm->pagetable, a2, (char *)&info, sizeof(info)) < 0)
      return -1;
    return 0;
  }
  }
  release(&lk);
  return -1;
}

static void
win32k_init(void)
{
  initlock(&lk, "win32k");
  initlock(&rawlk, "win32k-input");
  init_workqueue(&w32wq, "win32k", 1);
  init_work(&input_work, input_work_fn);
  for(int i = 0; i < MAXQ; i++)
    init_delayed_work(&queues[i].timer, timer_fn);
}

subsys_initcall("win32k", win32k_init);
