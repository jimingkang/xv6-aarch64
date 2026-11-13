// camview: the CSI camera in a window.
//
//   camview [-d]      -d: synthetic moving test frames instead of the camera
//                     (also used automatically when /dev/video0 is missing)
//
// A windowed program on the kernel window manager (win32k):
//
//   capture thread   read(/dev/video0) -> RAW10 frame
//                    camproc_rgb_raw()  -> demosaic, white balance, level,
//                                          gamma (shared with camshot)
//                    rows -> COLORREF picture (two, alternating)
//                    PostMessage(WM_FDREADY) -> this process's kernel queue
//   window thread    WM_FDREADY: invalidate the picture
//                    WM_PAINT: SetDIBitsToDevice() -> the kernel copies the
//                    bitmap into the window's surface (GDI in the kernel),
//                    EndPaint composes it onto the screen
//
// So the camera never touches the frame buffer: the picture is just window
// content, and can be dragged, covered and minimized like any other.

#include <uapi/types.h>
#include <uapi/stat.h>
#include <uapi/camera.h>
#include "user/user.h"
#include <fcntl.h>
#include "user/winapp.h"
#include "user/camproc.h"

#define IMG_W   320             // half-size demosaic of a 640x480 frame
#define IMG_H   240
#define BAR_H   30
#define ID_PAUSE 1
#define ID_WB    2
#define ID_STAT  3
#define WM_CAMDEMO (WM_USER + 1)  // capture thread fell back to demo frames

struct cam {
  HWND hwnd;
  int demo;
  int fd;
  uchar *frame;                 // one RAW10 capture
  COLORREF *pic[2];             // decoded pictures, alternately written
  int latest;                   // index of the newest complete picture, -1
  int paused, wb;
  int pending;                  // a WM_FDREADY is queued
  uint frames;
};

static struct cam cam;

// ---------------------------------------------------------------------------
// Synthetic RAW10 source: moving colour bars over a vertical gradient,
// mosaiced as RGGB and packed like the Unicam output.

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
    int shade = 400 + (y * 600) / ht;
    for(int x = 0; x < w; x += 4, s += 5){
      uchar lo = 0;
      for(int i = 0; i < 4; i++){
        int xx = x + i;
        const int *c = bars[((xx + frame_no * 8) / 80) % 8];
        int ch = (xx & 1) + 2 * (y & 1);
        int v = ch == 0 ? c[0] : ch == 3 ? c[2] : c[1];
        v = 16 + v * shade / 1000;
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
// Capture thread.

struct rowctx {
  COLORREF *dst;
  int w;
};

static void
put_row(void *arg, int y, const uchar *rgb, int w)
{
  struct rowctx *r = arg;
  if(y >= IMG_H)
    return;
  COLORREF *d = r->dst + y * r->w;
  for(int x = 0; x < w && x < r->w; x++, rgb += 3)
    d[x] = RGB(rgb[0], rgb[1], rgb[2]);
}

static void
capture_main(void *arg)
{
  struct cam *c = arg;
  int back = 0, failures = 0;

  for(;;){
    if(__atomic_load_n(&c->paused, __ATOMIC_ACQUIRE)){
      usleep(100000);
      continue;
    }
    int n;
    if(c->demo){
      demo_frame(c->frame, c->frames);
      n = sizeof(struct cam_frame_hdr) +
          ((struct cam_frame_hdr *)c->frame)->data_bytes;
      usleep(40000);            // ~25 fps, like a camera would deliver
    } else {
      n = read(c->fd, c->frame, CAM_READ_MAX);
    }
    struct cam_frame_hdr *h = (struct cam_frame_hdr *)c->frame;
    if(n < (int)sizeof(*h) || h->magic != CAM_MAGIC ||
       n != (int)(sizeof(*h) + h->data_bytes)){
      // /dev/video0 exists but no sensor answered (QEMU, unplugged
      // camera): fall back to the test feed rather than stay black.
      if(!c->demo && c->frames == 0 && ++failures >= 3){
        c->demo = 1;
        PostMessage(c->hwnd, WM_CAMDEMO, 0, 0);
      }
      usleep(200000);
      continue;
    }
    struct rowctx rc = { c->pic[back], IMG_W };
    if(camproc_rgb_raw(h, c->frame + sizeof(*h), 1,
                       __atomic_load_n(&c->wb, __ATOMIC_ACQUIRE),
                       put_row, &rc) < 0){
      usleep(200000);
      continue;
    }
    __atomic_store_n(&c->latest, back, __ATOMIC_RELEASE);
    back ^= 1;
    // Only this thread writes frames and sets pending, so plain atomic
    // loads and stores suffice (no read-modify-write helpers needed).
    __atomic_store_n(&c->frames, c->frames + 1, __ATOMIC_RELEASE);
    // One wake-up at a time: if the window has not caught up, the next
    // paint simply shows the newest picture.
    if(!__atomic_load_n(&c->pending, __ATOMIC_ACQUIRE)){
      __atomic_store_n(&c->pending, 1, __ATOMIC_RELEASE);
      PostMessage(c->hwnd, WM_FDREADY, 0, 0);
    }
  }
}

// ---------------------------------------------------------------------------
// The window.

static LRESULT
CamProc(HWND w, uint msg, WPARAM wp, LPARAM lp)
{
  switch(msg){
  case WM_CREATE:
    CreateWindow("BUTTON", "Pause", WS_CHILD | WS_VISIBLE, 4, IMG_H + 3,
                 64, 24, w, ID_PAUSE, 0);
    CreateWindow("BUTTON", "WB on", WS_CHILD | WS_VISIBLE, 72, IMG_H + 3,
                 64, 24, w, ID_WB, 0);
    CreateWindow("STATIC", "", WS_CHILD | WS_VISIBLE, 144, IMG_H + 7,
                 IMG_W - 148, 18, w, ID_STAT, 0);
    SetTimer(w, 1, 1000);
    return 0;
  case WM_CAMDEMO:
    SetWindowText(w, "Camera (demo: no sensor)");
    return 0;
  case WM_FDREADY: {
    __atomic_store_n(&cam.pending, 0, __ATOMIC_RELEASE);
    RECT pic = { 0, 0, IMG_W, IMG_H };
    InvalidateRect(w, &pic, 0);
    return 0;
  }
  case WM_TIMER: {
    static uint last;
    uint now = __atomic_load_n(&cam.frames, __ATOMIC_ACQUIRE);
    char s[80];
    strcpy(s, cam.demo ? "demo  " : "OV5647  ");
    itoa_(s + strlen(s), now - last);
    strcpy(s + strlen(s), " fps");
    if(cam.paused)
      strcpy(s + strlen(s), "  (paused)");
    last = now;
    SetWindowText(GetDlgItem(w, ID_STAT), s);
    return 0;
  }
  case WM_COMMAND:
    if(LOWORD(wp) == ID_PAUSE){
      int p = !cam.paused;
      __atomic_store_n(&cam.paused, p, __ATOMIC_RELEASE);
      SetWindowText(GetDlgItem(w, ID_PAUSE), p ? "Resume" : "Pause");
    } else if(LOWORD(wp) == ID_WB){
      int v = !cam.wb;
      __atomic_store_n(&cam.wb, v, __ATOMIC_RELEASE);
      SetWindowText(GetDlgItem(w, ID_WB), v ? "WB on" : "WB off");
    }
    return 0;
  case WM_PAINT: {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(w, &ps);
    int i = __atomic_load_n(&cam.latest, __ATOMIC_ACQUIRE);
    RECT pic = { 0, 0, IMG_W, IMG_H };
    if(i < 0){
      FillRect(dc, &pic, RGB(0, 0, 0));
      SetTextColor(dc, CLR_HILIGHT);
      DrawText(dc, "waiting for the camera...", -1, &pic, DT_CENTER);
    } else {
      SetDIBitsToDevice(dc, 0, 0, IMG_W, IMG_H, cam.pic[i]);
    }
    RECT line = { 0, IMG_H, IMG_W, IMG_H + 1 };
    FillRect(dc, &line, CLR_SHADOW);
    EndPaint(w, &ps);
    return 0;
  }
  case WM_DESTROY:
    PostQuitMessage(0);         // exit() also stops the capture thread
    return 0;
  }
  return DefWindowProc(w, msg, wp, lp);
}

int
main(int argc, char **argv)
{
  thread_t th;

  cam.latest = -1;
  cam.wb = 1;
  cam.fd = -1;
  cam.demo = argc > 1 && strcmp(argv[1], "-d") == 0;
  if(!cam.demo && (cam.fd = open("/dev/video0", O_RDWR)) < 0)
    cam.demo = 1;               // no camera here (QEMU): show the test feed
  cam.frame = malloc(CAM_READ_MAX);
  cam.pic[0] = malloc(IMG_W * IMG_H * sizeof(COLORREF));
  cam.pic[1] = malloc(IMG_W * IMG_H * sizeof(COLORREF));
  if(!cam.frame || !cam.pic[0] || !cam.pic[1]){
    printf("camview: out of memory\n");
    exit(1);
  }
  memset(cam.pic[0], 0, IMG_W * IMG_H * sizeof(COLORREF));
  memset(cam.pic[1], 0, IMG_W * IMG_H * sizeof(COLORREF));

  if(WinInit() < 0)
    exit(1);
  WNDCLASS wc = { "CamView", CamProc, CLR_FACE, 0 };
  RegisterClass(&wc);
  cam.hwnd = CreateWindow("CamView", cam.demo ? "Camera (demo)" : "Camera",
                          WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT,
                          CW_USEDEFAULT, IMG_W + 2 * 3,
                          IMG_H + BAR_H + 18 + 2 * 3, 0, 0, 0);
  if(!cam.hwnd)
    exit(1);
  // Capture starts once the window has its kernel handle (PostMessage).
  if(thread_create(&th, capture_main, &cam) < 0){
    printf("camview: cannot start the capture thread\n");
    exit(1);
  }
  run_message_loop();
  exit(0);
}
