// gui: a small desktop on the HDMI frame buffer (/dev/fb0).
//
//   gui            start the desktop; "Exit" on the taskbar (or Ctrl+Q)
//                  returns to the text console
//
// Everything is drawn in user space, the way an X server or a Wayland
// compositor sits on top of Linux fbdev/evdev:
//
//   /dev/fb0            mmap()ed frame buffer; ioctl(FBIOGET_VSCREENINFO)
//                       says where red/green/blue sit in a 32-bit pixel
//   /dev/input/eventN   USB HID boot keyboard and mouse (struct input_event);
//                       EVIOCGRAB keeps the keys away from the serial console
//   pty_open()          the terminal window runs /bin/sh on a pseudo-terminal
//
// The scene is composed in a back buffer in RAM and only the changed
// rectangle is copied to the frame buffer (which the kernel maps
// write-combining), so a moving cursor costs a few kilobytes, not a frame.
// Glyphs are the 8x16 lat1-16 font in font8x16.h.

#include <uapi/types.h>
#include <uapi/stat.h>
#include "user/user.h"
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <uapi/fb.h>
#include <uapi/input.h>
#include "user/font8x16.h"

#define MAX_INPUT   4
#define TERM_COLS   64
#define TERM_ROWS   20
#define TITLE_H     20
#define BAR_H       22          // top bar
#define TASK_H      26          // taskbar at the bottom
#define BORDER      2

// ---------------------------------------------------------------------------
// Pixels.

static int W, H;                // screen size
static uint32 *fb;              // mmap()ed frame buffer
static int fb_stride;           // pixels per frame-buffer line
static uint32 *bb;              // back buffer, W*H, same pixel format
static int roff, goff, boff;    // channel bit offsets from the kernel

static uint32
rgb(int r, int g, int b)
{
  return ((uint32)r << roff) | ((uint32)g << goff) | ((uint32)b << boff);
}

// Colours, built once the pixel format is known.
static uint32 C_DESK, C_DESK2, C_BAR, C_BARTXT, C_WIN, C_TITLE, C_TITLE_IN,
              C_TITLETXT, C_TERMBG, C_TERMFG, C_BLACK, C_WHITE, C_SHADOW,
              C_BTN, C_BTNHI, C_CLOSE, C_ACCENT;
static uint32 ansi[8];

static void
init_colors(void)
{
  C_DESK     = rgb(0x1d, 0x5a, 0x7a);
  C_DESK2    = rgb(0x0e, 0x2c, 0x40);
  C_BAR      = rgb(0x20, 0x22, 0x28);
  C_BARTXT   = rgb(0xe8, 0xe8, 0xe8);
  C_WIN      = rgb(0xd8, 0xd8, 0xd4);
  C_TITLE    = rgb(0x2f, 0x6f, 0xb3);
  C_TITLE_IN = rgb(0x7a, 0x84, 0x90);
  C_TITLETXT = rgb(0xff, 0xff, 0xff);
  C_TERMBG   = rgb(0x10, 0x12, 0x16);
  C_TERMFG   = rgb(0xc8, 0xd0, 0xc8);
  C_BLACK    = rgb(0, 0, 0);
  C_WHITE    = rgb(0xff, 0xff, 0xff);
  C_SHADOW   = rgb(0x08, 0x18, 0x24);
  C_BTN      = rgb(0x3a, 0x3e, 0x48);
  C_BTNHI    = rgb(0x4f, 0x7f, 0xc0);
  C_CLOSE    = rgb(0xd0, 0x4a, 0x3c);
  C_ACCENT   = rgb(0x7f, 0xd0, 0x8f);
  ansi[0] = rgb(0x30, 0x30, 0x30); ansi[1] = rgb(0xe0, 0x50, 0x50);
  ansi[2] = rgb(0x60, 0xd0, 0x60); ansi[3] = rgb(0xe0, 0xc0, 0x50);
  ansi[4] = rgb(0x60, 0x90, 0xf0); ansi[5] = rgb(0xd0, 0x70, 0xd0);
  ansi[6] = rgb(0x50, 0xc8, 0xd0); ansi[7] = C_TERMFG;
}

struct rect { int x0, y0, x1, y1; };    // half-open [x0,x1) x [y0,y1)
static struct rect dirty;               // what changed since the last flush
static struct rect clip;                // drawing is limited to this

static void
damage(int x, int y, int w, int h)
{
  if(w <= 0 || h <= 0)
    return;
  if(dirty.x1 <= dirty.x0){
    dirty.x0 = x; dirty.y0 = y; dirty.x1 = x + w; dirty.y1 = y + h;
  } else {
    if(x < dirty.x0) dirty.x0 = x;
    if(y < dirty.y0) dirty.y0 = y;
    if(x + w > dirty.x1) dirty.x1 = x + w;
    if(y + h > dirty.y1) dirty.y1 = y + h;
  }
}

static void
fill(int x, int y, int w, int h, uint32 c)
{
  int x0 = x < clip.x0 ? clip.x0 : x, y0 = y < clip.y0 ? clip.y0 : y;
  int x1 = x + w > clip.x1 ? clip.x1 : x + w;
  int y1 = y + h > clip.y1 ? clip.y1 : y + h;
  for(int j = y0; j < y1; j++){
    uint32 *p = bb + j * W;
    for(int i = x0; i < x1; i++)
      p[i] = c;
  }
}

static void
frame(int x, int y, int w, int h, uint32 c)
{
  fill(x, y, w, 1, c);
  fill(x, y + h - 1, w, 1, c);
  fill(x, y, 1, h, c);
  fill(x + w - 1, y, 1, h, c);
}

static void
glyph(int x, int y, int ch, uint32 fg, uint32 bg, int opaque)
{
  const uchar *g = 0;
  if(ch >= FONT_FIRST && ch <= FONT_LAST)
    g = font8x16[ch - FONT_FIRST];
  for(int r = 0; r < FONT_H; r++){
    int yy = y + r;
    if(yy < clip.y0 || yy >= clip.y1)
      continue;
    uchar bits = g ? g[r] : 0;
    uint32 *p = bb + yy * W;
    for(int c = 0; c < FONT_W; c++){
      int xx = x + c;
      if(xx < clip.x0 || xx >= clip.x1)
        continue;
      if(bits & (0x80 >> c))
        p[xx] = fg;
      else if(opaque)
        p[xx] = bg;
    }
  }
}

static void
text(int x, int y, const char *s, uint32 fg)
{
  for(; *s; s++, x += FONT_W)
    glyph(x, y, *s, fg, 0, 0);
}

// Copy the dirty rectangle of the back buffer to the screen.
static void
flush(void)
{
  if(dirty.x1 <= dirty.x0)
    return;
  int x0 = dirty.x0 < 0 ? 0 : dirty.x0, y0 = dirty.y0 < 0 ? 0 : dirty.y0;
  int x1 = dirty.x1 > W ? W : dirty.x1, y1 = dirty.y1 > H ? H : dirty.y1;
  for(int y = y0; y < y1; y++)
    memmove(fb + y * fb_stride + x0, bb + y * W + x0, (x1 - x0) * 4);
  dirty.x0 = dirty.x1 = 0;
}

// ---------------------------------------------------------------------------
// Terminal emulator: a TERM_COLS x TERM_ROWS cell grid fed by the pty master.

struct term {
  char ch[TERM_ROWS][TERM_COLS];
  uchar fg[TERM_ROWS][TERM_COLS];
  int cx, cy;
  int color;                    // current ANSI foreground 0..7
  int esc;                      // 0 normal, 1 after ESC, 2 inside CSI
  int par[4], npar;
  int master;                   // pty master fd, -1 when no shell
  int pid;
};

static struct term term;

static void
term_clear(struct term *t)
{
  memset(t->ch, ' ', sizeof(t->ch));
  memset(t->fg, 7, sizeof(t->fg));
  t->cx = t->cy = 0;
}

static void
term_scroll(struct term *t)
{
  memmove(t->ch[0], t->ch[1], (TERM_ROWS - 1) * TERM_COLS);
  memmove(t->fg[0], t->fg[1], (TERM_ROWS - 1) * TERM_COLS);
  memset(t->ch[TERM_ROWS - 1], ' ', TERM_COLS);
  memset(t->fg[TERM_ROWS - 1], 7, TERM_COLS);
}

static void
term_newline(struct term *t)
{
  t->cx = 0;
  if(++t->cy >= TERM_ROWS){
    t->cy = TERM_ROWS - 1;
    term_scroll(t);
  }
}

static void
term_csi(struct term *t, char final)
{
  int n = t->npar ? t->par[0] : 0;
  switch(final){
  case 'm':
    for(int i = 0; i < (t->npar ? t->npar : 1); i++){
      int p = t->npar ? t->par[i] : 0;
      if(p == 0 || p == 39)
        t->color = 7;
      else if(p >= 30 && p <= 37)
        t->color = p - 30;
    }
    break;
  case 'K':
    for(int x = t->cx; x < TERM_COLS; x++)
      t->ch[t->cy][x] = ' ';
    break;
  case 'J':
    if(n == 2)
      term_clear(t);
    break;
  case 'H':
    t->cy = (t->npar > 0 && t->par[0] > 0 ? t->par[0] - 1 : 0);
    t->cx = (t->npar > 1 && t->par[1] > 0 ? t->par[1] - 1 : 0);
    if(t->cy >= TERM_ROWS) t->cy = TERM_ROWS - 1;
    if(t->cx >= TERM_COLS) t->cx = TERM_COLS - 1;
    break;
  case 'A': t->cy -= n ? n : 1; if(t->cy < 0) t->cy = 0; break;
  case 'B': t->cy += n ? n : 1; if(t->cy >= TERM_ROWS) t->cy = TERM_ROWS - 1; break;
  case 'C': t->cx += n ? n : 1; if(t->cx >= TERM_COLS) t->cx = TERM_COLS - 1; break;
  case 'D': t->cx -= n ? n : 1; if(t->cx < 0) t->cx = 0; break;
  }
}

static void
term_putc(struct term *t, int c)
{
  if(t->esc == 1){
    if(c == '['){
      t->esc = 2;
      t->npar = 0;
      t->par[0] = 0;
    } else {
      t->esc = 0;
    }
    return;
  }
  if(t->esc == 2){
    if(c >= '0' && c <= '9'){
      if(t->npar == 0)
        t->npar = 1;
      t->par[t->npar - 1] = t->par[t->npar - 1] * 10 + (c - '0');
    } else if(c == ';'){
      if(t->npar == 0)
        t->npar = 1;
      if(t->npar < 4)
        t->par[t->npar++] = 0;
    } else {
      t->esc = 0;
      term_csi(t, c);
    }
    return;
  }
  switch(c){
  case 0x1b: t->esc = 1; return;
  case '\r': t->cx = 0; return;
  case '\n': term_newline(t); return;
  case '\b': if(t->cx > 0) t->cx--; return;
  case '\t':
    do term_putc(t, ' '); while(t->cx % 8);
    return;
  case 0x07: return;
  }
  if(c < ' ' || c > '~')
    return;
  if(t->cx >= TERM_COLS)
    term_newline(t);
  t->ch[t->cy][t->cx] = c;
  t->fg[t->cy][t->cx] = t->color;
  t->cx++;
}

static void
term_puts(struct term *t, const char *s)
{
  while(*s)
    term_putc(t, *s++);
}

// Fork /bin/sh with the pty slave as its stdin/stdout/stderr.
static void
term_spawn(struct term *t)
{
  int fd[2];
  if(t->master >= 0)
    return;
  if(pty_open(fd) < 0){
    term_puts(t, "gui: pty_open failed\r\n");
    return;
  }
  int pid = fork();
  if(pid < 0){
    close(fd[0]);
    close(fd[1]);
    term_puts(t, "gui: fork failed\r\n");
    return;
  }
  if(pid == 0){
    close(fd[0]);
    for(int i = 0; i < 3; i++){
      close(i);
      dup(fd[1]);
    }
    if(fd[1] > 2)
      close(fd[1]);
    // The GUI's input and pty fds are close-on-exec free in xv6; close the
    // ones a shell must not inherit.
    for(int i = 3; i < 16; i++)
      close(i);
    char *argv[] = { "sh", 0 };
    exec("/bin/sh", argv);
    printf("gui: cannot exec /bin/sh\n");
    exit(127);
  }
  close(fd[1]);
  t->master = fd[0];
  t->pid = pid;
  term_puts(t, "\033[32mxv6 terminal\033[0m: /bin/sh on a pty\r\n");
}

static void
term_hangup(struct term *t)
{
  if(t->master >= 0)
    close(t->master);
  t->master = -1;
  if(t->pid > 0){
    kill(t->pid);
    waitpid(t->pid, 0, 0);
  }
  t->pid = 0;
  term_puts(t, "\r\n\033[33m[shell exited; click Terminal to restart]\033[0m\r\n");
}

// ---------------------------------------------------------------------------
// Windows.

enum { WIN_TERM, WIN_SYS, WIN_ABOUT, NWIN };

struct window {
  const char *title;
  int x, y, w, h;               // outer rectangle, title bar included
  int visible;
};

static struct window win[NWIN];
static int zorder[NWIN];        // zorder[NWIN-1] is the top (focused) window
static int nz;

static int
term_client_w(void) { return TERM_COLS * FONT_W + 8; }
static int
term_client_h(void) { return TERM_ROWS * FONT_H + 8; }

static void
win_init(void)
{
  win[WIN_TERM].title = "Terminal - /bin/sh";
  win[WIN_TERM].w = term_client_w() + 2 * BORDER;
  win[WIN_TERM].h = term_client_h() + TITLE_H + BORDER;
  win[WIN_TERM].x = 16;
  win[WIN_TERM].y = BAR_H + 34;

  win[WIN_SYS].title = "System";
  win[WIN_SYS].w = 240;
  win[WIN_SYS].h = 150;
  win[WIN_SYS].x = W - 240 - 12;
  win[WIN_SYS].y = BAR_H + 12;

  win[WIN_ABOUT].title = "About";
  win[WIN_ABOUT].w = 300;
  win[WIN_ABOUT].h = 120;
  win[WIN_ABOUT].x = (W - 300) / 2;
  win[WIN_ABOUT].y = (H - 120) / 2;

  for(int i = 0; i < NWIN; i++)
    if(win[i].x + win[i].w > W)
      win[i].x = W - win[i].w > 0 ? W - win[i].w : 0;
  nz = 0;
  zorder[nz++] = WIN_SYS;
  zorder[nz++] = WIN_TERM;
  win[WIN_SYS].visible = win[WIN_TERM].visible = 1;
}

static int
focused(void)
{
  for(int i = nz - 1; i >= 0; i--)
    if(win[zorder[i]].visible)
      return zorder[i];
  return -1;
}

static void
win_damage(int id)
{
  // Shadow included.
  damage(win[id].x, win[id].y, win[id].w + 4, win[id].h + 4);
}

static void
raise_win(int id)
{
  int j = 0;
  for(int i = 0; i < nz; i++)
    if(zorder[i] != id)
      zorder[j++] = zorder[i];
  zorder[j++] = id;
  nz = j;
  win[id].visible = 1;
  win_damage(id);
}

// ---------------------------------------------------------------------------
// System information for the System window, refreshed once a second.

static char sysline[5][40];

static uint64
now_us(void)
{
  return clock_us();
}

static void
fmt_hms(char *buf, uint64 sec)
{
  int h = sec / 3600, m = (sec / 60) % 60, s = sec % 60;
  buf[0] = '0' + (h / 10) % 10; buf[1] = '0' + h % 10; buf[2] = ':';
  buf[3] = '0' + m / 10; buf[4] = '0' + m % 10; buf[5] = ':';
  buf[6] = '0' + s / 10; buf[7] = '0' + s % 10; buf[8] = 0;
}

// Find "Key:   1234 kB" in /proc/meminfo.
static int
meminfo(const char *buf, const char *key)
{
  int n = strlen(key);
  for(const char *p = buf; *p; ){
    if(strncmp(p, key, n) == 0 && p[n] == ':'){
      p += n + 1;
      while(*p == ' ')
        p++;
      return atoi(p);
    }
    while(*p && *p != '\n')
      p++;
    if(*p)
      p++;
  }
  return -1;
}

static void
put_kb(char *dst, const char *label, int kb)
{
  char num[16];
  int i = 0;
  strcpy(dst, label);
  if(kb < 0){
    strcpy(dst + strlen(dst), "?");
    return;
  }
  int mb = kb / 1024;
  if(mb == 0){ num[i++] = '0'; }
  while(mb > 0){ num[i++] = '0' + mb % 10; mb /= 10; }
  int d = strlen(dst);
  while(i > 0)
    dst[d++] = num[--i];
  strcpy(dst + d, " MB");
}

static void
sys_refresh(void)
{
  static char buf[1024];
  char hms[12];
  int fd, n = 0;

  fmt_hms(hms, now_us() / 1000000);
  strcpy(sysline[0], "Uptime:   ");
  strcpy(sysline[0] + strlen(sysline[0]), hms);
  buf[0] = 0;
  if((fd = open("/proc/meminfo", O_RDONLY)) >= 0){
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    buf[n > 0 ? n : 0] = 0;
  }
  put_kb(sysline[1], "MemTotal: ", meminfo(buf, "MemTotal"));
  put_kb(sysline[2], "MemFree:  ", meminfo(buf, "MemFree"));
  strcpy(sysline[3], "Display:  ");
  {
    char *d = sysline[3] + strlen(sysline[3]);
    int v[2] = { W, H };
    for(int k = 0; k < 2; k++){
      char num[8]; int i = 0, x = v[k];
      do { num[i++] = '0' + x % 10; x /= 10; } while(x);
      while(i) *d++ = num[--i];
      if(k == 0) *d++ = 'x';
    }
    strcpy(d, " 32bpp");
  }
  strcpy(sysline[4], "Board:    Raspberry Pi 3B");
  if(win[WIN_SYS].visible)
    win_damage(WIN_SYS);
}

// ---------------------------------------------------------------------------
// Scene.

static int mouse_x, mouse_y, mouse_seen;
static int mouse_buttons;
static int drag_win = -1, drag_dx, drag_dy;

struct button { const char *label; int x, y, w, h; };
enum { BTN_TERM, BTN_SYS, BTN_ABOUT, BTN_EXIT, NBTN };
static struct button task[NBTN];

static void
task_init(void)
{
  const char *labels[NBTN] = { "Terminal", "System", "About", "Exit" };
  int x = 6;
  for(int i = 0; i < NBTN; i++){
    task[i].label = labels[i];
    task[i].w = strlen(labels[i]) * FONT_W + 16;
    task[i].h = TASK_H - 6;
    task[i].y = H - TASK_H + 3;
    task[i].x = i == BTN_EXIT ? W - task[i].w - 6 : x;
    x += task[i].w + 6;
  }
}

static int
inside(int x, int y, int rx, int ry, int rw, int rh)
{
  return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static void
draw_desktop(void)
{
  // Vertical gradient between two blues, with the channels interpolated
  // through the pixel format so it is right on RGB and BGR alike.
  for(int y = BAR_H; y < H - TASK_H; y++){
    int t = (y - BAR_H) * 256 / (H - TASK_H - BAR_H);
    int r = (0x1d * (256 - t) + 0x0e * t) >> 8;
    int g = (0x5a * (256 - t) + 0x2c * t) >> 8;
    int b = (0x7a * (256 - t) + 0x40 * t) >> 8;
    fill(0, y, W, 1, rgb(r, g, b));
  }
  // Wordmark in the lower right corner, drawn twice as large.
  const char *mark = "xv6 / AArch64";
  int len = strlen(mark);
  int bx = W - len * 16 - 24, by = H - TASK_H - 48;
  for(int i = 0; i < len; i++){
    int ch = mark[i];
    if(ch < FONT_FIRST || ch > FONT_LAST)
      continue;
    const uchar *g = font8x16[ch - FONT_FIRST];
    for(int r = 0; r < FONT_H; r++)
      for(int c = 0; c < FONT_W; c++)
        if(g[r] & (0x80 >> c))
          fill(bx + i * 16 + c * 2, by + r * 2, 2, 2, rgb(0x3d, 0x7c, 0x9c));
  }
}

static void
draw_bar(void)
{
  struct timespec ts;
  char hms[12], clock[32];

  fill(0, 0, W, BAR_H, C_BAR);
  fill(0, BAR_H - 1, W, 1, C_BLACK);
  fill(8, 6, 10, 10, C_ACCENT);
  text(24, 3, "xv6 desktop", C_BARTXT);
  // Wall-clock time when the kernel has one (SNTP), else time since boot.
  if(clock_gettime(CLOCK_REALTIME, &ts) == 0 && ts.tv_sec > 1600000000){
    fmt_hms(hms, ts.tv_sec % 86400);
    strcpy(clock, hms);
    strcpy(clock + 8, " UTC");
  } else {
    fmt_hms(hms, now_us() / 1000000);
    strcpy(clock, "up ");
    strcpy(clock + 3, hms);
  }
  text(W - strlen(clock) * FONT_W - 10, 3, clock, C_BARTXT);
}

static void
draw_taskbar(void)
{
  fill(0, H - TASK_H, W, TASK_H, C_BAR);
  fill(0, H - TASK_H, W, 1, rgb(0x50, 0x54, 0x60));
  int f = focused();
  for(int i = 0; i < NBTN; i++){
    struct button *b = &task[i];
    int active = (i == BTN_TERM && f == WIN_TERM) ||
                 (i == BTN_SYS && f == WIN_SYS) ||
                 (i == BTN_ABOUT && f == WIN_ABOUT);
    uint32 c = i == BTN_EXIT ? C_CLOSE : active ? C_BTNHI : C_BTN;
    fill(b->x, b->y, b->w, b->h, c);
    frame(b->x, b->y, b->w, b->h, rgb(0x60, 0x66, 0x70));
    text(b->x + 8, b->y + 2, b->label, C_WHITE);
  }
}

static void
draw_term_client(int x, int y)
{
  fill(x, y, term_client_w(), term_client_h(), C_TERMBG);
  for(int r = 0; r < TERM_ROWS; r++)
    for(int c = 0; c < TERM_COLS; c++){
      int ch = term.ch[r][c];
      if(ch != ' ')
        glyph(x + 4 + c * FONT_W, y + 4 + r * FONT_H, ch,
              ansi[term.fg[r][c] & 7], 0, 0);
    }
  // Block cursor.
  if(term.master >= 0 && focused() == WIN_TERM){
    int cx = term.cx >= TERM_COLS ? TERM_COLS - 1 : term.cx;
    int px = x + 4 + cx * FONT_W, py = y + 4 + term.cy * FONT_H;
    fill(px, py, FONT_W, FONT_H, C_TERMFG);
    int ch = term.ch[term.cy][cx];
    if(ch != ' ')
      glyph(px, py, ch, C_TERMBG, 0, 0);
  }
}

static void
draw_window(int id)
{
  struct window *w = &win[id];
  int top = focused() == id;

  fill(w->x + 4, w->y + 4, w->w, w->h, C_SHADOW);
  fill(w->x, w->y, w->w, w->h, C_WIN);
  frame(w->x, w->y, w->w, w->h, C_BLACK);
  fill(w->x + 1, w->y + 1, w->w - 2, TITLE_H - 1, top ? C_TITLE : C_TITLE_IN);
  text(w->x + 8, w->y + 2, w->title, C_TITLETXT);
  // Close box.
  int bx = w->x + w->w - 18, by = w->y + 3;
  fill(bx, by, 14, 14, C_CLOSE);
  glyph(bx + 3, by - 1, 'x', C_WHITE, 0, 0);

  int cx = w->x + BORDER, cy = w->y + TITLE_H;
  if(id == WIN_TERM){
    draw_term_client(cx, cy);
  } else if(id == WIN_SYS){
    for(int i = 0; i < 5; i++)
      text(cx + 10, cy + 10 + i * 22, sysline[i], C_BLACK);
  } else if(id == WIN_ABOUT){
    text(cx + 12, cy + 12, "xv6 on Raspberry Pi 3B (AArch64)", C_BLACK);
    text(cx + 12, cy + 34, "fbdev + evdev + pty, drawn in", C_BLACK);
    text(cx + 12, cy + 52, "user space by /bin/gui.", C_BLACK);
    text(cx + 12, cy + 74, "Ctrl+Q or Exit: back to text", rgb(0x40, 0x40, 0x40));
  }
}

// 12x19 arrow: 'X' outline, '.' fill.
static const char *cursor_img[] = {
  "X           ",
  "XX          ",
  "X.X         ",
  "X..X        ",
  "X...X       ",
  "X....X      ",
  "X.....X     ",
  "X......X    ",
  "X.......X   ",
  "X........X  ",
  "X.........X ",
  "X......XXXXX",
  "X...X..X    ",
  "X..XX..X    ",
  "X.X  X..X   ",
  "XX   X..X   ",
  "X     X..X  ",
  "      X..X  ",
  "       XX   ",
};
#define CURSOR_W 12
#define CURSOR_H 19

static void
draw_cursor(void)
{
  if(!mouse_seen)
    return;
  for(int r = 0; r < CURSOR_H; r++)
    for(int c = 0; c < CURSOR_W; c++){
      char p = cursor_img[r][c];
      if(p == 'X')
        fill(mouse_x + c, mouse_y + r, 1, 1, C_BLACK);
      else if(p == '.')
        fill(mouse_x + c, mouse_y + r, 1, 1, C_WHITE);
    }
}

// Recompose the dirty rectangle: every layer is drawn clipped to it.
static void
compose(void)
{
  if(dirty.x1 <= dirty.x0)
    return;
  clip = dirty;
  if(clip.x0 < 0) clip.x0 = 0;
  if(clip.y0 < 0) clip.y0 = 0;
  if(clip.x1 > W) clip.x1 = W;
  if(clip.y1 > H) clip.y1 = H;
  draw_desktop();
  for(int i = 0; i < nz; i++)
    if(win[zorder[i]].visible)
      draw_window(zorder[i]);
  draw_bar();
  draw_taskbar();
  draw_cursor();
  clip.x0 = clip.y0 = 0;
  clip.x1 = W;
  clip.y1 = H;
}

static void
damage_all(void)
{
  damage(0, 0, W, H);
}

static void
damage_cursor(void)
{
  damage(mouse_x, mouse_y, CURSOR_W, CURSOR_H);
}

static void
damage_term(void)
{
  if(win[WIN_TERM].visible)
    damage(win[WIN_TERM].x + BORDER, win[WIN_TERM].y + TITLE_H,
           term_client_w(), term_client_h());
}

// ---------------------------------------------------------------------------
// Input.

static int infd[MAX_INPUT] = { -1, -1, -1, -1 };
static int shift, ctrl, capslock;
static int quit;

static void
input_scan(void)
{
  char path[] = "/dev/input/event0";
  for(int i = 0; i < MAX_INPUT; i++){
    if(infd[i] >= 0)
      continue;
    path[sizeof(path) - 2] = '0' + i;
    infd[i] = open(path, O_RDONLY | O_NONBLOCK);
    if(infd[i] >= 0)
      ioctl(infd[i], EVIOCGRAB, 1);
  }
}

static void
input_close(int i)
{
  if(infd[i] < 0)
    return;
  ioctl(infd[i], EVIOCGRAB, 0);
  close(infd[i]);
  infd[i] = -1;
}

// Linux key codes 2..53 to ASCII, unshifted and shifted.
static const char keymap[] =
  "\0\0001234567890-=\b\tqwertyuiop[]\n\0asdfghjkl;'`\0\\zxcvbnm,./";
static const char shiftmap[] =
  "\0\0!@#$%^&*()_+\b\tQWERTYUIOP{}\n\0ASDFGHJKL:\"~\0|ZXCVBNM<>?";

static void
send_term(const char *s, int n)
{
  if(term.master >= 0 && focused() == WIN_TERM)
    write(term.master, s, n);
}

static void
key(int code, int down)
{
  if(code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT){ shift = down; return; }
  if(code == KEY_LEFTCTRL || code == KEY_RIGHTCTRL){ ctrl = down; return; }
  if(!down)
    return;
  if(code == KEY_CAPSLOCK){ capslock ^= 1; return; }
  switch(code){
  case KEY_UP:    send_term("\033[A", 3); return;
  case KEY_DOWN:  send_term("\033[B", 3); return;
  case KEY_RIGHT: send_term("\033[C", 3); return;
  case KEY_LEFT:  send_term("\033[D", 3); return;
  case KEY_ESC:   send_term("\033", 1); return;
  case KEY_SPACE: send_term(" ", 1); return;
  }
  if(code < 0 || code >= (int)sizeof(keymap) - 1 || keymap[code] == 0)
    return;
  char c = keymap[code];
  int up = shift;
  if(c >= 'a' && c <= 'z')
    up ^= capslock;
  if(up)
    c = shiftmap[code];
  if(ctrl && (c | 0x20) >= 'a' && (c | 0x20) <= 'z'){
    if((c | 0x20) == 'q'){
      quit = 1;
      return;
    }
    c = (c | 0x20) - 'a' + 1;
  }
  send_term(&c, 1);
}

static void
close_window(int id)
{
  win[id].visible = 0;
  win_damage(id);
  damage(0, H - TASK_H, W, TASK_H);
  int f = focused();
  if(f >= 0)
    win_damage(f);
}

static void
click(int x, int y)
{
  // Taskbar.
  for(int i = 0; i < NBTN; i++){
    struct button *b = &task[i];
    if(!inside(x, y, b->x, b->y, b->w, b->h))
      continue;
    if(i == BTN_EXIT){
      quit = 1;
    } else {
      int id = i == BTN_TERM ? WIN_TERM : i == BTN_SYS ? WIN_SYS : WIN_ABOUT;
      int old = focused();
      raise_win(id);
      if(old >= 0)
        win_damage(old);
      if(id == WIN_TERM)
        term_spawn(&term);
    }
    damage(0, H - TASK_H, W, TASK_H);
    return;
  }
  // Windows, top first.
  for(int i = nz - 1; i >= 0; i--){
    int id = zorder[i];
    struct window *w = &win[id];
    if(!w->visible || !inside(x, y, w->x, w->y, w->w, w->h))
      continue;
    int old = focused();
    if(old != id){
      raise_win(id);
      if(old >= 0)
        win_damage(old);
      damage(0, H - TASK_H, W, TASK_H);
    }
    if(inside(x, y, w->x + w->w - 18, w->y + 3, 14, 14)){
      close_window(id);
    } else if(y < w->y + TITLE_H){
      drag_win = id;
      drag_dx = x - w->x;
      drag_dy = y - w->y;
    }
    return;
  }
}

static void
mouse_move(int dx, int dy)
{
  damage_cursor();
  if(!mouse_seen){
    mouse_seen = 1;
    mouse_x = W / 2;
    mouse_y = H / 2;
  }
  mouse_x += dx;
  mouse_y += dy;
  if(mouse_x < 0) mouse_x = 0;
  if(mouse_y < 0) mouse_y = 0;
  if(mouse_x > W - 1) mouse_x = W - 1;
  if(mouse_y > H - 1) mouse_y = H - 1;
  if(drag_win >= 0){
    struct window *w = &win[drag_win];
    win_damage(drag_win);
    w->x = mouse_x - drag_dx;
    w->y = mouse_y - drag_dy;
    if(w->y < BAR_H) w->y = BAR_H;
    if(w->y > H - TASK_H - TITLE_H) w->y = H - TASK_H - TITLE_H;
    if(w->x > W - 40) w->x = W - 40;
    if(w->x + w->w < 40) w->x = 40 - w->w;
    win_damage(drag_win);
  }
  damage_cursor();
}

static void
input_read(int i)
{
  struct input_event ev[32];
  int n = read(infd[i], ev, sizeof(ev));
  if(n <= 0){
    // Unplugged (a non-blocking read with nothing queued also lands here,
    // but poll() said readable, so treat it as gone).
    input_close(i);
    return;
  }
  int dx = 0, dy = 0;
  for(int k = 0; k < n / (int)sizeof(ev[0]); k++){
    struct input_event *e = &ev[k];
    if(e->type == EV_REL){
      if(e->code == REL_X) dx += e->value;
      else if(e->code == REL_Y) dy += e->value;
    } else if(e->type == EV_KEY){
      if(e->code == BTN_LEFT){
        if(dx || dy){ mouse_move(dx, dy); dx = dy = 0; }
        if(!mouse_seen)
          mouse_move(0, 0);
        if(e->value){
          mouse_buttons |= 1;
          click(mouse_x, mouse_y);
        } else {
          mouse_buttons &= ~1;
          drag_win = -1;
        }
      } else if(e->code < BTN_LEFT){
        key(e->code, e->value);
      }
    }
  }
  if(dx || dy)
    mouse_move(dx, dy);
}

// ---------------------------------------------------------------------------

static int
fb_open(void)
{
  struct fb_var_screeninfo var;
  struct fb_fix_screeninfo fix;
  int fd = open("/dev/fb0", O_RDWR);

  if(fd < 0){
    printf("gui: cannot open /dev/fb0\n");
    return -1;
  }
  if(ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0 ||
     ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0 || var.bits_per_pixel != 32){
    printf("gui: /dev/fb0 is not a 32 bpp frame buffer\n");
    close(fd);
    return -1;
  }
  W = var.xres;
  H = var.yres;
  fb_stride = fix.line_length / 4;
  roff = var.red.offset;
  goff = var.green.offset;
  boff = var.blue.offset;
  if(roff == boff){             // a kernel without channel offsets: 0xRRGGBB
    roff = 16;
    goff = 8;
    boff = 0;
  }
  fb = mmap(0, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if(fb == MAP_FAILED){
    printf("gui: mmap /dev/fb0 failed\n");
    return -1;
  }
  bb = malloc(W * H * 4);
  if(bb == 0){
    printf("gui: no memory for a %dx%d back buffer\n", W, H);
    return -1;
  }
  printf("gui: %dx%d, red at bit %d, blue at bit %d\n", W, H, roff, boff);
  return 0;
}

int
main(int argc, char **argv)
{
  struct pollfd pfd[MAX_INPUT + 1];
  int map[MAX_INPUT + 1];
  uint64 last_tick = 0, last_scan = 0;

  if(fb_open() < 0)
    exit(1);
  init_colors();
  clip.x1 = W;
  clip.y1 = H;
  term.master = -1;
  term.color = 7;
  term_clear(&term);
  win_init();
  task_init();
  sys_refresh();
  input_scan();
  term_spawn(&term);
  damage_all();

  while(!quit){
    int n = 0;
    for(int i = 0; i < MAX_INPUT; i++)
      if(infd[i] >= 0){
        pfd[n].fd = infd[i];
        pfd[n].events = POLLIN;
        pfd[n].revents = 0;
        map[n++] = i;
      }
    if(term.master >= 0){
      pfd[n].fd = term.master;
      pfd[n].events = POLLIN;
      pfd[n].revents = 0;
      map[n++] = -1;
    }
    compose();
    flush();
    poll(pfd, n, 100);

    for(int k = 0; k < n; k++){
      if(!(pfd[k].revents & (POLLIN | POLLERR)))
        continue;
      if(map[k] >= 0){
        input_read(map[k]);
      } else {
        char buf[512];
        int r = read(term.master, buf, sizeof(buf));
        if(r <= 0){
          term_hangup(&term);
        } else {
          for(int j = 0; j < r; j++)
            term_putc(&term, buf[j]);
        }
        damage_term();
      }
    }

    uint64 t = now_us();
    if(t - last_tick >= 1000000){
      last_tick = t;
      damage(0, 0, W, BAR_H);
      sys_refresh();
    }
    if(t - last_scan >= 2000000){
      last_scan = t;
      input_scan();             // keyboard or mouse plugged in later
    }
    // Reap a shell that exited without closing the pty first.
    if(term.pid > 0 && waitpid(term.pid, 0, WNOHANG) == term.pid){
      term.pid = 0;
      term_hangup(&term);
      damage_term();
    }
  }

  for(int i = 0; i < MAX_INPUT; i++)
    input_close(i);
  if(term.master >= 0){
    close(term.master);
    term.master = -1;
  }
  if(term.pid > 0){
    kill(term.pid);
    waitpid(term.pid, 0, 0);
  }
  // Leave a clean screen behind.
  for(int y = 0; y < H; y++)
    memset(fb + y * fb_stride, 0, W * 4);
  printf("gui: bye\n");
  exit(0);
}
