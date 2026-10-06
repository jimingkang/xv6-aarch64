// camshot: capture one frame from /dev/video0 and save it as BMP or JPEG.
//
//   camshot [-o OUT.bmp|OUT.jpg] [-q QUALITY] [-r OUT.raw] [-i IN.raw]
//           [-t PATTERN] [-s] [-w]
//
//   -o  output selected by .bmp/.jpg/.jpeg suffix (default /boot/camera.bmp)
//   -q  JPEG quality 1..100 (default 75)
//   -r  also save the raw frame: struct cam_frame_hdr + packed RAW10
//   -i  convert a raw frame saved with -r instead of capturing
//   -t  OV5647 test pattern: 1 colour bars, 2 colour squares, 3 random
//   -s  half size, 320x240: one RGB pixel per 2x2 Bayer block
//   -w  skip gray-world white balance
//
// Files: the native xv6 file system holds at most 268 KB per file
// (MAXFILE = 12 + 256 blocks), so a 640x480 BMP (900 KB) or a raw frame
// (375 KB) must go to the FAT32 boot partition mounted at /boot.  A
// 320x240 BMP (225 KB, -s) fits anywhere.
//
// Processing, all in integers (this toolchain builds user code without FP):
// unpack CSI-2 RAW10 -> subtract black level -> bilinear demosaic (any of
// the four Bayer orders, taken from the frame header) -> gray-world white
// balance -> auto level (99th percentile) -> gamma 2.2.  Width, height and
// line length also come from the header, so other sensors and modes work.
#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"
#include "user/jpeg.h"
#include "kernel/camera.h"

#define BLACK 16                      // black level, 10-bit units (OV5647)
#define BMP_BATCH_ROWS 16             // amortise FAT32/syscall overhead

enum { R, G, B };
static int W, H;                      // from the frame header
static int bayer[4];                  // colour at (x&1) + 2*(y&1)

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

static ushort *px;                    // unpacked 10-bit mosaic, W*H

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

// Set the Bayer table from a CAM_FMT_*; returns -1 if unknown.
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

// Bilinear: at an R or B site the other one is on the diagonals and G on
// the cross; at a G site the horizontal and vertical neighbours carry R and
// B in an order that depends on the row.
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

static void
put32(uchar *p, uint v)
{
  p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
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

static int
endswith(char *path, char *suffix)
{
  int n = strlen(path), m = strlen(suffix);
  return n >= m && strcmp(path + n - m, suffix) == 0;
}

static void
usage(void)
{
  fprintf(2, "usage: camshot [-o OUT.bmp|OUT.jpg] [-q QUALITY] "
             "[-r OUT.raw] [-i IN.raw] [-t PATTERN] [-s] [-w]\n");
  exit(1);
}

int
main(int argc, char *argv[])
{
  char *out = "/boot/camera.bmp", *raw = 0, *in = 0;
  int pattern = 0, wb = 1, half = 0, quality = 75, fd, n;

  printf("camshot: converter jpeg420-v1 bmp-batch16\n");

  for(int i = 1; i < argc; i++){
    if(strcmp(argv[i], "-o") == 0 && i + 1 < argc) out = argv[++i];
    else if(strcmp(argv[i], "-q") == 0 && i + 1 < argc) quality = atoi(argv[++i]);
    else if(strcmp(argv[i], "-r") == 0 && i + 1 < argc) raw = argv[++i];
    else if(strcmp(argv[i], "-i") == 0 && i + 1 < argc) in = argv[++i];
    else if(strcmp(argv[i], "-t") == 0 && i + 1 < argc) pattern = atoi(argv[++i]);
    else if(strcmp(argv[i], "-w") == 0) wb = 0;
    else if(strcmp(argv[i], "-s") == 0) half = 1;
    else usage();
  }
  if(pattern < 0 || pattern > 3 || quality < 1 || quality > 100)
    usage();

  uchar *frame = malloc(CAM_READ_MAX);
  if(frame == 0){
    fprintf(2, "camshot: out of memory\n");
    exit(1);
  }
  if(in){
    if((fd = open(in, O_RDONLY)) < 0){
      fprintf(2, "camshot: cannot open %s\n", in);
      exit(1);
    }
    n = 0;
    for(int m; n < (int)CAM_READ_MAX &&
               (m = read(fd, frame + n, CAM_READ_MAX - n)) > 0; )
      n += m;
    close(fd);
  } else {
    if((fd = open("/dev/video0", O_RDWR)) < 0){
      fprintf(2, "camshot: cannot open /dev/video0\n");
      exit(1);
    }
    char cmd[] = "pattern=0";
    cmd[8] = '0' + pattern;
    // Always send it, so a pattern left over from an interrupted run is
    // cleared; only a pattern that was asked for must be accepted.
    int control_result = write(fd, cmd, 9);
    if(control_result != 9 && pattern){
      fprintf(2, "camshot: pattern=%d control failed (write returned %d)\n",
              pattern, control_result);
      exit(1);
    }
    printf("camshot: capturing...\n");
    n = read(fd, frame, CAM_READ_MAX);
    if(pattern){
      cmd[8] = '0';
      write(fd, cmd, 9);
    }
    close(fd);
  }
  struct cam_frame_hdr *hdr = (struct cam_frame_hdr *)frame;
  if(n < (int)sizeof(*hdr) || hdr->magic != CAM_MAGIC){
    fprintf(2, "camshot: capture failed (see kernel log)\n");
    exit(1);
  }
  W = hdr->width;
  H = hdr->height;
  if(n != (int)(sizeof(*hdr) + hdr->data_bytes) || W < 4 || H < 4 ||
     W % 4 || H % 2 || hdr->bytesperline < W * 10 / 8 ||
     hdr->data_bytes < hdr->bytesperline * H || set_bayer(hdr->format) < 0){
    fprintf(2, "camshot: unsupported frame %dx%d format %d (%d bytes)\n",
            W, H, hdr->format, n);
    exit(1);
  }
  printf("camshot: frame received %dx%d, %d bytes\n", W, H, n);
  if((px = malloc(W * H * sizeof(ushort))) == 0){
    fprintf(2, "camshot: out of memory\n");
    exit(1);
  }

  if(raw){
    int rfd = open(raw, O_CREATE | O_WRONLY | O_TRUNC);
    if(rfd < 0 || write(rfd, frame, n) != n)
      fprintf(2, "camshot: cannot write %s (%d bytes; native xv6 files are "
              "limited to 268 KB, use /boot/...)\n", raw, n);
    else
      printf("camshot: raw frame -> %s (%d bytes)\n", raw, n);
    if(rfd >= 0)
      close(rfd);
  }

  uint seq = hdr->sequence;
  printf("camshot: unpacking RAW10...\n");
  unpack_raw10(frame + sizeof(*hdr), hdr->bytesperline);
  free(frame);                      // hdr points into frame

  uint gain_r = 256, gain_b = 256;
  uint level = 256;
  if(pattern){
    // Sensor-generated colour bars are already calibrated.  Fixed unity
    // gains validate capture, conversion and storage without allowing an
    // optional image-enhancement pass to obscure that test.
    printf("camshot: sensor test pattern; fixed WB/level\n");
  } else {
    // Gray world and the G histogram share one pass over the real mosaic.
    printf("camshot: calculating white balance and level...\n");
    uint64 sum[3] = { 0, 0, 0 };
    static uint hist[1024];
    for(int y = 0; y < H; y++){
      for(int x = 0; x < W; x++){
        int c = colour(x, y);
        int value = px[y * W + x];
        if(c < R || c > B || value < 0 || value >= 1024){
          fprintf(2, "camshot: corrupt mosaic row=%d col=%d c=%d value=%d\n",
                  y, x, c, value);
          exit(1);
        }
        sum[c] += value;
        if(c == G)
          hist[value]++;
      }
      if((y & 63) == 63)
        printf("camshot: statistics %d/%d rows\n", y + 1, H);
    }
    uint64 sum_r = sum[R], sum_g = sum[G], sum_b = sum[B];
    if(wb && sum_r && sum_b){
      gain_r = (uint)(sum_g * 128 / sum_r);        // G counts 2 pixels
      gain_b = (uint)(sum_g * 128 / sum_b);
      if(gain_r < 128) gain_r = 128;
      if(gain_r > 1024) gain_r = 1024;
      if(gain_b < 128) gain_b = 128;
      if(gain_b > 1024) gain_b = 1024;
    }

    // Map the 99th percentile of G to full scale (at most 8x).
    uint want = (W * H / 2) / 100, seen = 0;
    int p99 = 1023;
    while(p99 > 0 && seen + hist[p99] < want)
      seen += hist[p99--];
    if(p99 < 128)
      p99 = 128;
    level = 1023 * 256 / p99;                      // 8.8
  }

  int ow = half ? W / 2 : W, oh = half ? H / 2 : H;
  struct convert conversion = { half, gain_r, gain_b, level };
  if(endswith(out, ".jpg") || endswith(out, ".jpeg")){
    printf("camshot: encoding baseline JPEG 4:2:0 quality=%d -> %s\n",
           quality, out);
    int ofd = open(out, O_CREATE | O_WRONLY | O_TRUNC);
    uint jpeg_bytes = 0;
    if(ofd < 0 || jpeg_encode(ofd, ow, oh, quality, output_pixel,
                              &conversion, &jpeg_bytes) < 0){
      fprintf(2, "camshot: JPEG write failed: %s\n", out);
      if(ofd >= 0)
        close(ofd);
      exit(1);
    }
    close(ofd);
    printf("camshot: %dx%d frame %d, JPEG quality=%d, %d bytes -> %s\n",
           ow, oh, seq, quality, jpeg_bytes, out);
    exit(0);
  }
  if(!endswith(out, ".bmp")){
    fprintf(2, "camshot: output suffix must be .bmp, .jpg or .jpeg\n");
    exit(1);
  }

  // 24-bit BMP, rows bottom-up, BGR, each row padded to 4 bytes.
  int row = (ow * 3 + 3) & ~3, img = row * oh;
  uchar bmp[54];
  memset(bmp, 0, sizeof(bmp));
  bmp[0] = 'B'; bmp[1] = 'M';
  put32(bmp + 2, 54 + img);
  put32(bmp + 10, 54);
  put32(bmp + 14, 40);
  put32(bmp + 18, ow);
  put32(bmp + 22, oh);
  bmp[26] = 1;
  bmp[28] = 24;
  put32(bmp + 34, img);
  put32(bmp + 38, 2835);                          // 72 dpi
  put32(bmp + 42, 2835);

  printf("camshot: preparing output %s (%d bytes)...\n", out, 54 + img);
  int ofd = open(out, O_CREATE | O_WRONLY | O_TRUNC);
  if(ofd < 0){
    fprintf(2, "camshot: cannot create %s\n", out);
    exit(1);
  }
  printf("camshot: output opened; writing BMP header...\n");
  if(write(ofd, bmp, 54) != 54){
    fprintf(2, "camshot: cannot write BMP header to %s\n", out);
    close(ofd);
    exit(1);
  }
  uchar *lines = malloc(row * BMP_BATCH_ROWS);
  if(lines == 0){
    fprintf(2, "camshot: out of memory for BMP output buffer\n");
    close(ofd);
    exit(1);
  }
  int batch_rows = 0;
  printf("camshot: converting and writing %s (%d bytes)...\n", out,
         54 + img);
  for(int y = oh - 1; y >= 0; y--){
    uchar *line = lines + batch_rows * row;
    memset(line, 0, row);
    for(int x = 0; x < ow; x++){
      uchar r, g, b;
      output_pixel(&conversion, x, y, &r, &g, &b);
      line[x * 3 + 0] = b;
      line[x * 3 + 1] = g;
      line[x * 3 + 2] = r;
    }
    batch_rows++;
    if((batch_rows == BMP_BATCH_ROWS || y == 0) &&
       write(ofd, lines, batch_rows * row) != batch_rows * row){
      fprintf(2, "camshot: write to %s failed (%d bytes; native xv6 files "
              "are limited to 268 KB: use /boot/... or -s)\n", out,
              54 + img);
      exit(1);
    }
    if(batch_rows == BMP_BATCH_ROWS || y == 0)
      batch_rows = 0;
  }
  close(ofd);
  free(lines);
  printf("camshot: %dx%d frame %d, WB R x%d.%d B x%d.%d, level x%d.%d -> %s\n",
         ow, oh, seq, gain_r / 256, gain_r % 256 * 10 / 256,
         gain_b / 256, gain_b % 256 * 10 / 256,
         level / 256, level % 256 * 10 / 256, out);
  exit(0);
}
