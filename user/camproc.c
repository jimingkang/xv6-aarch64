// Camera frame -> JPEG conversion shared by camshot and camserver.
//
// This is the image pipeline originally written inside user/camshot.c, moved
// here so the HTTP streaming server can reuse it.  All arithmetic is integer
// (the user toolchain builds without FP): unpack CSI-2 RAW10 -> subtract black
// level -> bilinear demosaic -> gray-world white balance -> auto level (99th
// percentile) -> gamma 2.2 -> baseline JPEG 4:2:0.
#include "kernel/types.h"
#include "kernel/camera.h"
#include "user/user.h"
#include "user/jpeg.h"
#include "user/camproc.h"

#define BLACK 16                      // black level, 10-bit units (OV5647)

enum { R, G, B };
static int W, H;                      // from the frame header
static int bayer[4];                  // colour at (x&1) + 2*(y&1)
static ushort *px;                    // unpacked 10-bit mosaic, W*H
static int px_size;                   // W*H for which px is allocated
static uint hist[1024];               // green histogram for auto level

static const uchar gamma22[256] = {
    0,  21,  28,  34,  39,  43,  46,  50,  53,  56,  59,  61,  64,  66,  68,  70,
   72,  74,  76,  78,  80,  82,  84,  85,  87,  89,  90,  92,  93,  95,  96,  98,
   99, 101, 102, 103, 105, 106, 107, 109, 110, 111, 112, 114, 115, 116, 117, 118,
  119, 120, 122, 123, 124, 125, 126, 127, 128, 129, 130, 131, 132, 133, 134, 135,
  136, 137, 138, 139, 140, 141, 142, 143, 144, 144, 145, 146, 147, 148, 149, 150,
  151, 151, 152, 153, 154, 155, 156, 156, 157, 158, 159, 160, 160, 161, 162, 163,
  164, 164, 165, 166, 167, 167, 168, 169, 170, 170, 171, 172, 173, 173, 174, 175,
  175, 176, 177, 178, 178, 179, 180, 180, 181, 182, 182, 183, 184, 184, 185, 186,
  186, 187, 188, 188, 189, 190, 190, 191, 192, 192, 193, 194, 194, 195, 195, 196,
  197, 197, 198, 199, 199, 200, 200, 201, 202, 202, 203, 203, 204, 205, 205, 206,
  206, 207, 207, 208, 209, 209, 210, 210, 211, 212, 212, 213, 213, 214, 214, 215,
  215, 216, 217, 217, 218, 218, 219, 219, 220, 220, 221, 221, 222, 223, 223, 224,
  224, 225, 225, 226, 226, 227, 227, 228, 228, 229, 229, 230, 230, 231, 231, 232,
  232, 233, 233, 234, 234, 235, 235, 236, 236, 237, 237, 238, 238, 239, 239, 240,
  240, 241, 241, 242, 242, 243, 243, 244, 244, 245, 245, 246, 246, 247, 247, 248,
  248, 249, 249, 249, 250, 250, 251, 251, 252, 252, 253, 253, 254, 254, 255, 255,
};

static void
unpack_raw10(const uchar *src, int bpl)
{
  for(int y = 0; y < H; y++){
    const uchar *s = src + y * bpl;
    ushort *d = px + y * W;
    for(int x = 0; x < W; x += 4, s += 5){
      uchar lo = s[4];
      for(int i = 0; i < 4; i++){
        int v = (s[i] << 2) | ((lo >> (2 * i)) & 3);
        d[x + i] = v > BLACK ? v - BLACK : 0;
      }
    }
  }
}

// Reflect at the borders so the neighbour keeps the same Bayer colour.
static inline int
P(int x, int y)
{
  if(x < 0) x = -x;
  if(x >= W) x = 2 * W - 2 - x;
  if(y < 0) y = -y;
  if(y >= H) y = 2 * H - 2 - y;
  return px[y * W + x];
}

static inline int
colour(int x, int y)
{
  return bayer[(x & 1) + 2 * (y & 1)];
}

static int
set_bayer(uint format)
{
  static const int orders[4][4] = {
    { B, G, G, R },                   // CAM_FMT_SBGGR10P
    { G, B, R, G },                   // CAM_FMT_SGBRG10P
    { G, R, B, G },                   // CAM_FMT_SGRBG10P
    { R, G, G, B },                   // CAM_FMT_SRGGB10P
  };
  if(format < CAM_FMT_SBGGR10P || format > CAM_FMT_SRGGB10P)
    return -1;
  for(int i = 0; i < 4; i++)
    bayer[i] = orders[format - CAM_FMT_SBGGR10P][i];
  return 0;
}

static void
demosaic(int x, int y, int *r, int *g, int *b)
{
  int cross = (P(x-1,y) + P(x+1,y) + P(x,y-1) + P(x,y+1)) / 4;
  int diag  = (P(x-1,y-1) + P(x+1,y-1) + P(x-1,y+1) + P(x+1,y+1)) / 4;
  int horiz = (P(x-1,y) + P(x+1,y)) / 2;
  int vert  = (P(x,y-1) + P(x,y+1)) / 2;
  int c = P(x, y);

  switch(colour(x, y)){
  case R: *r = c; *g = cross; *b = diag; break;
  case B: *b = c; *g = cross; *r = diag; break;
  default:
    *g = c;
    if(colour(x + 1, y) == R){ *r = horiz; *b = vert; }
    else { *b = horiz; *r = vert; }
  }
}

struct convert {
  int half;
  uint gain_r;
  uint gain_b;
  uint level;
};

static void
output_pixel(void *arg, int x, int y, uchar *red, uchar *green, uchar *blue)
{
  struct convert *c = arg;
  int r, g, b;
  if(c->half){
    int v[3] = { 0, 0, 0 };
    for(int i = 0; i < 4; i++)
      v[bayer[i]] += P(2 * x + (i & 1), 2 * y + (i >> 1));
    r = v[R];
    g = v[G] / 2;
    b = v[B];
  } else {
    demosaic(x, y, &r, &g, &b);
  }
  uint lr = ((uint)r * c->gain_r / 256) * c->level / 256 / 4;
  uint lg = (uint)g * c->level / 256 / 4;
  uint lb = ((uint)b * c->gain_b / 256) * c->level / 256 / 4;
  *red = gamma22[lr > 255 ? 255 : lr];
  *green = gamma22[lg > 255 ? 255 : lg];
  *blue = gamma22[lb > 255 ? 255 : lb];
}

static void
compute_wb_level(int wb, uint *gain_r, uint *gain_b, uint *level)
{
  uint64 sum[3] = { 0, 0, 0 };
  *gain_r = *gain_b = 256;
  *level = 256;
  memset(hist, 0, sizeof(hist));
  for(int y = 0; y < H; y++){
    for(int x = 0; x < W; x++){
      int c = colour(x, y);
      int value = px[y * W + x];      // unpack_raw10 clamps to 0..1023
      sum[c] += value;
      if(c == G)
        hist[value]++;
    }
  }
  uint64 sum_r = sum[R], sum_g = sum[G], sum_b = sum[B];
  if(wb && sum_r && sum_b){
    *gain_r = (uint)(sum_g * 128 / sum_r);        // G counts 2 pixels
    *gain_b = (uint)(sum_g * 128 / sum_b);
    if(*gain_r < 128) *gain_r = 128;
    if(*gain_r > 1024) *gain_r = 1024;
    if(*gain_b < 128) *gain_b = 128;
    if(*gain_b > 1024) *gain_b = 1024;
  }
  uint want = (W * H / 2) / 100, seen = 0;
  int p99 = 1023;
  while(p99 > 0 && seen + hist[p99] < want)
    seen += hist[p99--];
  if(p99 < 128)
    p99 = 128;
  *level = 1023 * 256 / p99;                      // 8.8
}

int
camproc_jpeg(uchar *dst, int cap, const uchar *frame, int n,
             int half, int wb, int quality, uint *written)
{
  struct cam_frame_hdr *hdr = (struct cam_frame_hdr *)frame;
  if(dst == 0 || cap <= 0 || frame == 0 ||
     n < (int)sizeof(*hdr) || hdr->magic != CAM_MAGIC)
    return -1;
  W = hdr->width;
  H = hdr->height;
  if(n != (int)(sizeof(*hdr) + hdr->data_bytes) || W < 4 || H < 4 ||
     W % 4 || H % 2 || hdr->bytesperline < W * 10 / 8 ||
     hdr->data_bytes < hdr->bytesperline * H || set_bayer(hdr->format) < 0)
    return -1;

  // (Re)allocate the mosaic scratch buffer only when the geometry changes.
  if(px == 0 || px_size != W * H){
    free(px);
    px = malloc(W * H * sizeof(ushort));
    px_size = px ? W * H : 0;
    if(px == 0)
      return -1;
  }

  unpack_raw10(frame + sizeof(*hdr), hdr->bytesperline);
  uint gain_r, gain_b, level;
  compute_wb_level(wb, &gain_r, &gain_b, &level);
  int ow = half ? W / 2 : W, oh = half ? H / 2 : H;
  struct convert conversion = { half, gain_r, gain_b, level };
  return jpeg_encode_mem(dst, cap, ow, oh, quality, output_pixel,
                         &conversion, written);
}
