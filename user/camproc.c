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
static uchar tone[3][1024];           // per-frame WB/level/gamma lookup

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
unpack_raw10_analyze(const uchar *src, int bpl, uint64 sum[3])
{
  sum[R] = sum[G] = sum[B] = 0;
  memset(hist, 0, sizeof(hist));
  for(int y = 0; y < H; y++){
    const uchar *s = src + y * bpl;
    ushort *d = px + y * W;
    for(int x = 0; x < W; x += 4, s += 5){
      uchar lo = s[4];
      for(int i = 0; i < 4; i++){
        int v = (s[i] << 2) | ((lo >> (2 * i)) & 3);
        v = v > BLACK ? v - BLACK : 0;
        d[x + i] = v;
        int c = bayer[((x + i) & 1) + 2 * (y & 1)];
        sum[c] += v;
        if(c == G)
          hist[v]++;
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
  int c, cross, diag, horiz, vert;
  int type = colour(x, y);

  // Almost every output pixel is interior.  Address its three rows directly
  // instead of calling P() 5--9 times (each P has four border branches and a
  // y*W multiply).  Border pixels retain the reflected-coordinate path below.
  if(x > 0 && x + 1 < W && y > 0 && y + 1 < H){
    const ushort *up = px + (y - 1) * W;
    const ushort *row = up + W;
    const ushort *down = row + W;
    c = row[x];
    if(type == R || type == B){
      cross = (row[x-1] + row[x+1] + up[x] + down[x]) / 4;
      diag = (up[x-1] + up[x+1] + down[x-1] + down[x+1]) / 4;
      if(type == R){ *r = c; *g = cross; *b = diag; }
      else { *b = c; *g = cross; *r = diag; }
    } else {
      horiz = (row[x-1] + row[x+1]) / 2;
      vert = (up[x] + down[x]) / 2;
      *g = c;
      if(colour(x + 1, y) == R){ *r = horiz; *b = vert; }
      else { *b = horiz; *r = vert; }
    }
    return;
  }

  c = P(x, y);
  if(type == R || type == B){
    cross = (P(x-1,y) + P(x+1,y) + P(x,y-1) + P(x,y+1)) / 4;
    diag = (P(x-1,y-1) + P(x+1,y-1) +
            P(x-1,y+1) + P(x+1,y+1)) / 4;
    if(type == R){ *r = c; *g = cross; *b = diag; }
    else { *b = c; *g = cross; *r = diag; }
  } else {
    horiz = (P(x-1,y) + P(x+1,y)) / 2;
    vert = (P(x,y-1) + P(x,y+1)) / 2;
    *g = c;
    if(colour(x + 1, y) == R){ *r = horiz; *b = vert; }
    else { *b = horiz; *r = vert; }
  }
}

struct convert {
  int half;
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
  // The per-frame gains, level and gamma curve are already folded into these
  // tables.  This removes six variable multiplies/shifts from every output
  // pixel while preserving the old integer truncation order exactly.
  *red = tone[R][r > 1023 ? 1023 : r];
  *green = tone[G][g > 1023 ? 1023 : g];
  *blue = tone[B][b > 1023 ? 1023 : b];
}

static void
build_tone(uint gain_r, uint gain_b, uint level)
{
  for(uint v = 0; v < 1024; v++){
    uint lr = (v * gain_r / 256) * level / 256 / 4;
    uint lg = v * level / 256 / 4;
    uint lb = (v * gain_b / 256) * level / 256 / 4;
    tone[R][v] = gamma22[lr > 255 ? 255 : lr];
    tone[G][v] = gamma22[lg > 255 ? 255 : lg];
    tone[B][v] = gamma22[lb > 255 ? 255 : lb];
  }
}

static void
compute_wb_level(int wb, const uint64 sum[3],
                 uint *gain_r, uint *gain_b, uint *level)
{
  *gain_r = *gain_b = 256;
  *level = 256;
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
camproc_jpeg_raw(uchar *dst, int cap, const struct cam_frame_hdr *hdr,
                 const uchar *raw, int half, int wb, int quality,
                 uint *written)
{
  if(dst == 0 || cap <= 0 || hdr == 0 || raw == 0 ||
     hdr->magic != CAM_MAGIC)
    return -1;
  W = hdr->width;
  H = hdr->height;
  if(W < 4 || H < 4 || W % 4 || H % 2 ||
     hdr->data_bytes > CAM_SLOT_BYTES ||
     hdr->bytesperline < W * 10 / 8 ||
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

  uint64 sum[3];
  unpack_raw10_analyze(raw, hdr->bytesperline, sum);
  uint gain_r, gain_b, level;
  compute_wb_level(wb, sum, &gain_r, &gain_b, &level);
  build_tone(gain_r, gain_b, level);
  int ow = half ? W / 2 : W, oh = half ? H / 2 : H;
  struct convert conversion = { half };
  return jpeg_encode_mem(dst, cap, ow, oh, quality, output_pixel,
                         &conversion, written);
}

int
camproc_jpeg(uchar *dst, int cap, const uchar *frame, int n,
             int half, int wb, int quality, uint *written)
{
  const struct cam_frame_hdr *hdr = (const struct cam_frame_hdr *)frame;
  if(frame == 0 || n < (int)sizeof(*hdr) ||
     n != (int)(sizeof(*hdr) + hdr->data_bytes))
    return -1;
  return camproc_jpeg_raw(dst, cap, hdr, frame + sizeof(*hdr),
                          half, wb, quality, written);
}
