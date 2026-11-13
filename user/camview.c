// camview: show the CSI camera live on the HDMI frame buffer.
//
//   camview [-f] [-w] [-p N] [-n FRAMES] [-d]
//     -f   full resolution: demosaic every pixel (default: half size, one
//          RGB pixel per 2x2 Bayer block, which is ~4x cheaper)
//     -w   no gray-world white balance
//     -p N OV5647 test pattern (1 colour bars, 2 squares, 3 random)
//     -n N stop after N frames (default: run until q / Esc)
//     -d   demo: no camera, feed a synthetic moving RAW10 frame through the
//          same pipeline (works in QEMU, which has no sensor)
//
// Pipeline per frame:
//   read(/dev/video0)  -> struct cam_frame_hdr + packed CSI-2 RAW10 (Unicam DMA)
//   camproc_rgb_raw()  -> black level, demosaic, white balance, auto level,
//                         gamma (the same code camshot/camserver use for JPEG)
//   row callback       -> convert to the fb0 pixel format, scale up to fill the
//                         screen, store straight into the mmap()ed frame buffer
//
// Quit with q or Esc on the USB keyboard (read from /dev/input/eventN), or
// q + Enter on the serial console.

#include <uapi/types.h>
#include <uapi/stat.h>
#include <uapi/camera.h>
#include "user/user.h"
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <uapi/fb.h>
#include <uapi/input.h>
#include "user/camproc.h"
#include "user/font8x16.h"

#define MAX_INPUT 4
#define KEY_Q     16

static uint32 *fb;
static uint32 *bb;                      // back buffer FW*FH: a frame is built
                                        // here, then copied in one pass, so
                                        // the screen tears far less
static int FW, FH, stride;              // screen size, pixels per fb line
static int roff, goff, boff;

static int
fb_setup(void)
{
  struct fb_var_screeninfo var;
  struct fb_fix_screeninfo fix;
  int fd = open("/dev/fb0", O_RDWR);

  if(fd < 0){
    printf("camview: cannot open /dev/fb0\n");
    return -1;
  }
  if(ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0 ||
     ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0 || var.bits_per_pixel != 32){
    printf("camview: /dev/fb0 is not a 32 bpp frame buffer\n");
    close(fd);
    return -1;
  }
  FW = var.xres;
  FH = var.yres;
  stride = fix.line_length / 4;
  roff = var.red.offset;
  goff = var.green.offset;
  boff = var.blue.offset;
  if(roff == boff){
    roff = 16;
    goff = 8;
    boff = 0;
  }
  fb = mmap(0, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if(fb == MAP_FAILED){
    printf("camview: mmap /dev/fb0 failed\n");
    return -1;
  }
  return 0;
}

static void
clear_screen(void)
{
  for(int y = 0; y < FH; y++)
    memset(fb + y * stride, 0, FW * 4);
  if(bb)
    memset(bb, 0, FW * FH * 4);
}

// Copy the finished frame to the screen, below the status line.
static void
present(void)
{
  for(int y = FONT_H; y < FH; y++)
    memmove(fb + y * stride, bb + y * FW, FW * 4);
}

static uint32
pixel(int r, int g, int b)
{
  return ((uint32)r << roff) | ((uint32)g << goff) | ((uint32)b << boff);
}

// ---------------------------------------------------------------------------
// Drawing a frame: camproc hands us one RGB row at a time.

struct view {
  int scale;                    // integer zoom so the image fills the screen
  int ox, oy;                   // top-left of the image on screen
  uint32 *line;                 // one scaled row in fb pixel format
};

static void
put_row(void *arg, int y, const uchar *rgb, int w)
{
  struct view *v = arg;
  int s = v->scale;
  uint32 *d = v->line;

  for(int x = 0; x < w; x++, rgb += 3){
    uint32 p = pixel(rgb[0], rgb[1], rgb[2]);
    for(int k = 0; k < s; k++)
      *d++ = p;
  }
  for(int k = 0; k < s; k++){
    int sy = v->oy + y * s + k;
    if(sy >= 0 && sy < FH)
      memmove(bb + sy * FW + v->ox, v->line, w * s * 4);
  }
}

static void
text(int x, int y, const char *s)
{
  uint32 fg = pixel(255, 255, 255), bg = pixel(0, 0, 0);
  for(; *s; s++, x += FONT_W){
    int ch = *s;
    const uchar *g = ch >= FONT_FIRST && ch <= FONT_LAST ?
                     font8x16[ch - FONT_FIRST] : 0;
    for(int r = 0; r < FONT_H; r++){
      if(y + r >= FH)
        break;
      uint32 *p = fb + (y + r) * stride + x;
      for(int c = 0; c < FONT_W && x + c < FW; c++)
        p[c] = g && (g[r] & (0x80 >> c)) ? fg : bg;
    }
  }
}

static char *
putnum(char *p, int v)
{
  char tmp[12];
  int n = 0;
  if(v < 0){
    *p++ = '-';
    v = -v;
  }
  do tmp[n++] = '0' + v % 10; while((v /= 10) && n < 11);
  while(n)
    *p++ = tmp[--n];
  return p;
}

static char *
putstr(char *p, const char *s)
{
  while(*s)
    *p++ = *s++;
  return p;
}

// ---------------------------------------------------------------------------
// Synthetic RAW10 source for -d: moving colour bars over a vertical
// gradient, mosaiced as RGGB and packed exactly like the Unicam output.

static void
demo_frame(uchar *buf, int frame_no)
{
  struct cam_frame_hdr *h = (struct cam_frame_hdr *)buf;
  int w = 640, ht = 480, bpl = w * 10 / 8;
  static const int bars[8][3] = {
    { 1023, 1023, 1023 }, { 1023, 1023, 0 }, { 0, 1023, 1023 },
    { 0, 1023, 0 }, { 1023, 0, 1023 }, { 1023, 0, 0 }, { 0, 0, 1023 },
    { 160, 160, 160 },
  };

  memset(h, 0, sizeof(*h));
  h->magic = CAM_MAGIC;
  h->width = w;
  h->height = ht;
  h->bytesperline = bpl;
  h->format = CAM_FMT_SRGGB10P;
  h->data_bytes = bpl * ht;
  h->sequence = frame_no;
  uchar *raw = buf + sizeof(*h);
  for(int y = 0; y < ht; y++){
    uchar *s = raw + y * bpl;
    int shade = 400 + (y * 600) / ht;           // brighter towards the bottom
    for(int x = 0; x < w; x += 4, s += 5){
      uchar lo = 0;
      for(int i = 0; i < 4; i++){
        int xx = x + i;
        const int *c = bars[((xx + frame_no * 8) / 80) % 8];
        int ch = (xx & 1) + 2 * (y & 1);      // RGGB: 0=R 1,2=G 3=B
        int v = ch == 0 ? c[0] : ch == 3 ? c[2] : c[1];
        v = 16 + v * shade / 1000;              // plus the black level
        if(v > 1023)
          v = 1023;
        s[i] = v >> 2;
        lo |= (v & 3) << (2 * i);
      }
      s[4] = lo;
    }
  }
}

// ---------------------------------------------------------------------------
// Quit keys.

static int infd[MAX_INPUT] = { -1, -1, -1, -1 };

static void
input_open(void)
{
  char path[] = "/dev/input/event0";
  for(int i = 0; i < MAX_INPUT; i++){
    path[sizeof(path) - 2] = '0' + i;
    infd[i] = open(path, O_RDONLY | O_NONBLOCK);
    // Grab it so q/Esc are not also typed into the shell on the console.
    if(infd[i] >= 0)
      ioctl(infd[i], EVIOCGRAB, 1);
  }
}

static int
quit_pressed(void)
{
  struct pollfd pfd[MAX_INPUT + 1];
  int map[MAX_INPUT + 1], n = 0, quit = 0;

  for(int i = 0; i < MAX_INPUT; i++)
    if(infd[i] >= 0){
      pfd[n].fd = infd[i];
      pfd[n].events = POLLIN;
      pfd[n].revents = 0;
      map[n++] = i;
    }
  pfd[n].fd = 0;                // serial console: "q" + Enter
  pfd[n].events = POLLIN;
  pfd[n].revents = 0;
  map[n++] = -1;
  if(poll(pfd, n, 0) <= 0)
    return 0;
  for(int k = 0; k < n; k++){
    if(!(pfd[k].revents & POLLIN))
      continue;
    if(map[k] < 0){
      char buf[32];
      int r = read(0, buf, sizeof(buf));
      for(int j = 0; j < r; j++)
        if(buf[j] == 'q' || buf[j] == 'Q')
          quit = 1;
    } else {
      struct input_event ev[16];
      int r = read(infd[map[k]], ev, sizeof(ev));
      if(r <= 0){
        close(infd[map[k]]);
        infd[map[k]] = -1;
        continue;
      }
      for(int j = 0; j < r / (int)sizeof(ev[0]); j++)
        if(ev[j].type == EV_KEY && ev[j].value == 1 &&
           (ev[j].code == KEY_Q || ev[j].code == KEY_ESC))
          quit = 1;
    }
  }
  return quit;
}

// ---------------------------------------------------------------------------

static void
usage(void)
{
  fprintf(2, "usage: camview [-f] [-w] [-p N] [-n FRAMES] [-d]\n");
  exit(1);
}

int
main(int argc, char **argv)
{
  int half = 1, wb = 1, pattern = 0, frames = -1, demo = 0, cam = -1;

  for(int i = 1; i < argc; i++){
    if(strcmp(argv[i], "-f") == 0) half = 0;
    else if(strcmp(argv[i], "-w") == 0) wb = 0;
    else if(strcmp(argv[i], "-d") == 0) demo = 1;
    else if(strcmp(argv[i], "-p") == 0 && i + 1 < argc) pattern = atoi(argv[++i]);
    else if(strcmp(argv[i], "-n") == 0 && i + 1 < argc) frames = atoi(argv[++i]);
    else usage();
  }

  uchar *frame = malloc(CAM_READ_MAX);
  struct view view;
  view.line = malloc(4096 * 4);
  if(frame == 0 || view.line == 0){
    printf("camview: out of memory\n");
    exit(1);
  }
  if(!demo){
    cam = open("/dev/video0", O_RDWR);
    if(cam < 0){
      printf("camview: cannot open /dev/video0 (no camera? try -d)\n");
      exit(1);
    }
    if(pattern){
      char cmd[] = "pattern=0";
      cmd[8] = '0' + pattern;
      if(write(cam, cmd, 9) != 9)
        printf("camview: pattern=%d not accepted\n", pattern);
    }
  }
  if(fb_setup() < 0)
    exit(1);
  if((bb = malloc(FW * FH * 4)) == 0){
    printf("camview: no memory for a back buffer\n");
    exit(1);
  }
  input_open();
  clear_screen();
  printf("camview: %s, %s; q or Esc quits\n", demo ? "demo frames" : "/dev/video0",
         half ? "half size" : "full size");

  uint64 t0 = clock_us(), last = t0;
  int shown = 0, fps10 = 0, geometry = 0;
  while(frames < 0 || shown < frames){
    int n;
    if(demo){
      demo_frame(frame, shown);
      n = sizeof(struct cam_frame_hdr) +
          ((struct cam_frame_hdr *)frame)->data_bytes;
    } else {
      n = read(cam, frame, CAM_READ_MAX);
    }
    struct cam_frame_hdr *h = (struct cam_frame_hdr *)frame;
    if(n < (int)sizeof(*h) || h->magic != CAM_MAGIC ||
       n != (int)(sizeof(*h) + h->data_bytes)){
      printf("camview: no frame from %s (%d)\n",
             demo ? "demo source" : "/dev/video0 -- is a camera connected?", n);
      break;
    }

    int ow = half ? h->width / 2 : h->width;
    int oh = half ? h->height / 2 : h->height;
    if(geometry != ow * 65536 + oh){
      // First frame (or the sensor mode changed): fit and centre it.
      geometry = ow * 65536 + oh;
      // The status line is drawn over the top of the picture.
      int sx = FW / ow, sy = FH / oh;
      view.scale = sx < sy ? sx : sy;
      if(view.scale < 1)
        view.scale = 1;
      if(ow * view.scale > 4096)
        view.scale = 4096 / ow;
      view.ox = (FW - ow * view.scale) / 2;
      view.oy = (FH - oh * view.scale) / 2;
      if(view.ox < 0) view.ox = 0;
      if(view.oy < 0) view.oy = 0;
      clear_screen();
    }
    if(camproc_rgb_raw(h, frame + sizeof(*h), half, wb, put_row, &view) < 0){
      printf("camview: unsupported frame %dx%d format %d\n",
             h->width, h->height, h->format);
      break;
    }
    present();
    shown++;

    uint64 now = clock_us();
    if(now - last >= 1000000 || shown == 1){
      fps10 = now > t0 ? (int)((uint64)shown * 10000000 / (now - t0)) : 0;
      last = now;
    }
    char status[96], *p = status;
    p = putstr(p, "camview ");
    p = putnum(p, h->width);
    *p++ = 'x';
    p = putnum(p, h->height);
    p = putstr(p, half ? " half x" : " full x");
    p = putnum(p, view.scale);
    p = putstr(p, "  ");
    p = putnum(p, fps10 / 10);
    *p++ = '.';
    p = putnum(p, fps10 % 10);
    p = putstr(p, " fps  frame ");
    p = putnum(p, shown);
    p = putstr(p, "   q/Esc: quit ");
    *p = 0;
    text(0, 0, status);

    if(quit_pressed())
      break;
  }

  uint64 t1 = clock_us();
  if(t1 > t0 && shown > 0){
    int f10 = (int)((uint64)shown * 10000000 / (t1 - t0));
    printf("camview: %d frames, %d.%d fps\n", shown, f10 / 10, f10 % 10);
  }
  clear_screen();
  if(cam >= 0)
    close(cam);
  for(int i = 0; i < MAX_INPUT; i++)
    if(infd[i] >= 0){
      ioctl(infd[i], EVIOCGRAB, 0);
      close(infd[i]);
    }
  exit(0);
}
