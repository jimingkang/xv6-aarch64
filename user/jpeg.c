// Small freestanding baseline JPEG encoder for xv6.
//
// It intentionally does not depend on floating point or libjpeg.  One 16x16
// MCU is converted to four Y blocks and one each of downsampled Cb/Cr, then
// transformed and emitted immediately.  Custom canonical Huffman tables keep
// the implementation compact while remaining standard JPEG.

#ifdef JPEG_HOST_TEST
#include <stdint.h>
#include <string.h>
#include <unistd.h>
typedef uint8_t uchar;
typedef uint16_t ushort;
typedef uint32_t uint;
typedef uint64_t uint64;
#else
#include "kernel/types.h"
#include "user/user.h"
#endif
#include "user/jpeg.h"

#define OUTBUF 4096

struct jw {
  int fd;         // fd-mode destination (memory mode uses mem below)
  uchar *mem;     // memory destination; when non-zero, fd is ignored
  int cap;        // memory capacity in bytes
  int error;
  uint total;     // bytes emitted so far (both modes)
  int used;       // fd mode: bytes buffered in out[]
  uchar out[OUTBUF];
  uint bits;
  int nbits;
};

static const uchar zigzag[64] = {
   0, 1, 8,16, 9, 2, 3,10,17,24,32,25,18,11, 4, 5,
  12,19,26,33,40,48,41,34,27,20,13, 6, 7,14,21,28,
  35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,
  58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63
};

static const uchar qbase_y[64] = {
  16,11,10,16,24,40,51,61, 12,12,14,19,26,58,60,55,
  14,13,16,24,40,57,69,56, 14,17,22,29,51,87,80,62,
  18,22,37,56,68,109,103,77, 24,35,55,64,81,104,113,92,
  49,64,78,87,103,121,120,101, 72,92,95,98,112,100,103,99
};

static const uchar qbase_c[64] = {
  17,18,24,47,99,99,99,99, 18,21,26,66,99,99,99,99,
  24,26,56,99,99,99,99,99, 47,66,99,99,99,99,99,99,
  99,99,99,99,99,99,99,99, 99,99,99,99,99,99,99,99,
  99,99,99,99,99,99,99,99, 99,99,99,99,99,99,99,99
};

// C(u)*cos((2*x+1)*u*pi/16), Q14.  C(0)=1/sqrt(2).
static const short dctm[8][8] = {
  {11585,11585,11585,11585,11585,11585,11585,11585},
  {16069,13623, 9102, 3196,-3196,-9102,-13623,-16069},
  {15137, 6270,-6270,-15137,-15137,-6270, 6270,15137},
  {13623,-3196,-16069,-9102, 9102,16069, 3196,-13623},
  {11585,-11585,-11585,11585,11585,-11585,-11585,11585},
  { 9102,-16069, 3196,13623,-13623,-3196,16069,-9102},
  { 6270,-15137,15137,-6270,-6270,15137,-15137, 6270},
  { 3196,-9102,13623,-16069,16069,-13623,9102,-3196}
};

static int
jflush(struct jw *w)
{
  if(w->mem)
    return w->error ? -1 : 0;   // memory mode: jbyte() wrote directly
  int done = 0;
  while(done < w->used){
    int n = write(w->fd, w->out + done, w->used - done);
    if(n <= 0){
      w->error = 1;
      return -1;
    }
    done += n;
  }
  w->total += w->used;
  w->used = 0;
  return 0;
}

static void
jbyte(struct jw *w, int value)
{
  if(w->error)
    return;
  if(w->mem){
    if(w->total >= w->cap){
      w->error = 1;
      return;
    }
    w->mem[w->total++] = value;
    return;
  }
  if(w->used == OUTBUF && jflush(w) < 0)
    return;
  w->out[w->used++] = value;
}

static void
jword(struct jw *w, int value)
{
  jbyte(w, value >> 8);
  jbyte(w, value);
}

static void
jmarker(struct jw *w, int marker)
{
  jbyte(w, 0xff);
  jbyte(w, marker);
}

static void
entropy_byte(struct jw *w, int value)
{
  jbyte(w, value);
  if((value & 255) == 255)
    jbyte(w, 0);                    // JPEG byte stuffing
}

static void
put_bits(struct jw *w, uint value, int count)
{
  if(count <= 0 || w->error)
    return;
  value &= (1U << count) - 1;
  w->bits = (w->bits << count) | value;
  w->nbits += count;
  while(w->nbits >= 8){
    w->nbits -= 8;
    entropy_byte(w, w->bits >> w->nbits);
    if(w->nbits)
      w->bits &= (1U << w->nbits) - 1;
    else
      w->bits = 0;
  }
}

static void
finish_bits(struct jw *w)
{
  if(w->nbits)
    put_bits(w, (1U << (8 - w->nbits)) - 1, 8 - w->nbits);
}

static int
category(int value)
{
  uint v = value < 0 ? -value : value;
  int n = 0;
  while(v){
    v >>= 1;
    n++;
  }
  return n;
}

static uint
amplitude(int value, int size)
{
  if(value >= 0)
    return value;
  return value + (1U << size) - 1;
}

static void
make_quant(uchar out[64], const uchar base[64], int quality)
{
  int scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
  for(int i = 0; i < 64; i++){
    int q = (base[i] * scale + 50) / 100;
    if(q < 1) q = 1;
    if(q > 255) q = 255;
    out[i] = q;
  }
}

static void
fdct_quant(const short input[64], const uchar quant[64], short output[64])
{
  int tmp[64];
  for(int y = 0; y < 8; y++)
    for(int u = 0; u < 8; u++){
      int sum = 0;
      for(int x = 0; x < 8; x++)
        sum += input[y * 8 + x] * dctm[u][x];
      tmp[y * 8 + u] = sum;
    }
  for(int v = 0; v < 8; v++)
    for(int u = 0; u < 8; u++){
      long long sum = 0;
      for(int y = 0; y < 8; y++)
        sum += (long long)tmp[y * 8 + u] * dctm[v][y];
      int value;
      if(sum >= 0)
        value = (sum + ((long long)1 << 29)) >> 30;
      else
        value = -((-sum + ((long long)1 << 29)) >> 30);
      int q = quant[v * 8 + u];
      if(value >= 0)
        value = (value + q / 2) / q;
      else
        value = -((-value + q / 2) / q);
      output[v * 8 + u] = value;
    }
}

// DC symbols 0..11 have four-bit canonical codes equal to the symbol.
// AC uses one eight-bit code per useful baseline symbol: EOB, ZRL, then all
// (run 0..15, size 1..10) combinations.  162/256 of the code space is used.
static void
encode_block(struct jw *w, const short input[64], const uchar quant[64],
             int *previous_dc)
{
  short coefficient[64];
  fdct_quant(input, quant, coefficient);

  int dc = coefficient[0];
  int diff = dc - *previous_dc;
  *previous_dc = dc;
  int size = category(diff);
  if(size > 11) size = 11;
  put_bits(w, size, 4);
  if(size)
    put_bits(w, amplitude(diff, size), size);

  int run = 0;
  for(int k = 1; k < 64; k++){
    int value = coefficient[zigzag[k]];
    if(value == 0){
      run++;
      continue;
    }
    while(run >= 16){
      put_bits(w, 1, 8);            // symbol index 1 is ZRL (0xf0)
      run -= 16;
    }
    if(value > 1023) value = 1023;
    if(value < -1023) value = -1023;
    size = category(value);
    int code = 2 + run * 10 + size - 1;
    put_bits(w, code, 8);
    put_bits(w, amplitude(value, size), size);
    run = 0;
  }
  if(run)
    put_bits(w, 0, 8);              // symbol index 0 is EOB
}

static void
write_headers(struct jw *w, int width, int height,
              const uchar qy[64], const uchar qc[64])
{
  jmarker(w, 0xd8);                 // SOI
  jmarker(w, 0xe0);                 // JFIF APP0
  jword(w, 16);
  jbyte(w, 'J'); jbyte(w, 'F'); jbyte(w, 'I'); jbyte(w, 'F'); jbyte(w, 0);
  jbyte(w, 1); jbyte(w, 1); jbyte(w, 0);
  jword(w, 1); jword(w, 1); jbyte(w, 0); jbyte(w, 0);

  jmarker(w, 0xdb);                 // two 8-bit quantization tables
  jword(w, 132);
  jbyte(w, 0);
  for(int i = 0; i < 64; i++) jbyte(w, qy[zigzag[i]]);
  jbyte(w, 1);
  for(int i = 0; i < 64; i++) jbyte(w, qc[zigzag[i]]);

  jmarker(w, 0xc0);                 // baseline SOF, Y 2x2 and Cb/Cr 1x1
  jword(w, 17); jbyte(w, 8); jword(w, height); jword(w, width); jbyte(w, 3);
  jbyte(w, 1); jbyte(w, 0x22); jbyte(w, 0);
  jbyte(w, 2); jbyte(w, 0x11); jbyte(w, 1);
  jbyte(w, 3); jbyte(w, 0x11); jbyte(w, 1);

  jmarker(w, 0xc4);                 // one DC and one AC Huffman table
  jword(w, 210);
  jbyte(w, 0x00);                   // DC table 0
  for(int len = 1; len <= 16; len++) jbyte(w, len == 4 ? 12 : 0);
  for(int i = 0; i < 12; i++) jbyte(w, i);
  jbyte(w, 0x10);                   // AC table 0
  for(int len = 1; len <= 16; len++) jbyte(w, len == 8 ? 162 : 0);
  jbyte(w, 0x00);                   // EOB
  jbyte(w, 0xf0);                   // ZRL
  for(int run = 0; run < 16; run++)
    for(int size = 1; size <= 10; size++)
      jbyte(w, (run << 4) | size);

  jmarker(w, 0xda);                 // SOS
  jword(w, 12); jbyte(w, 3);
  jbyte(w, 1); jbyte(w, 0x00);
  jbyte(w, 2); jbyte(w, 0x00);
  jbyte(w, 3); jbyte(w, 0x00);
  jbyte(w, 0); jbyte(w, 63); jbyte(w, 0);
}

static int
jpeg_encode_to(struct jw *w, int width, int height, int quality,
               jpeg_pixel_fn pixel, void *arg, uint *written)
{
  uchar qy[64], qc[64];
  uchar rgb[16][16][3];
  short yblock[64], cb[64], cr[64];
  int pred_y = 0, pred_cb = 0, pred_cr = 0;

  if(width <= 0 || height <= 0 || width > 65535 ||
     height > 65535 || quality < 1 || quality > 100 || pixel == 0)
    return -1;
  make_quant(qy, qbase_y, quality);
  make_quant(qc, qbase_c, quality);
  write_headers(w, width, height, qy, qc);

  for(int my = 0; my < height; my += 16){
    for(int mx = 0; mx < width; mx += 16){
      for(int y = 0; y < 16; y++)
        for(int x = 0; x < 16; x++){
          int sx = mx + x < width ? mx + x : width - 1;
          int sy = my + y < height ? my + y : height - 1;
          pixel(arg, sx, sy, &rgb[y][x][0], &rgb[y][x][1], &rgb[y][x][2]);
        }

      for(int by = 0; by < 2; by++)
        for(int bx = 0; bx < 2; bx++){
          for(int y = 0; y < 8; y++)
            for(int x = 0; x < 8; x++){
              uchar *p = rgb[by * 8 + y][bx * 8 + x];
              yblock[y * 8 + x] = ((77*p[0] + 150*p[1] + 29*p[2]) >> 8)-128;
            }
          encode_block(w, yblock, qy, &pred_y);
        }

      for(int y = 0; y < 8; y++)
        for(int x = 0; x < 8; x++){
          int sr = 0, sg = 0, sb = 0;
          for(int dy = 0; dy < 2; dy++)
            for(int dx = 0; dx < 2; dx++){
              uchar *p = rgb[y * 2 + dy][x * 2 + dx];
              sr += p[0]; sg += p[1]; sb += p[2];
            }
          int r = (sr + 2) >> 2, g = (sg + 2) >> 2, b = (sb + 2) >> 2;
          cb[y * 8 + x] = (-43*r - 85*g + 128*b) >> 8;
          cr[y * 8 + x] = (128*r - 107*g - 21*b) >> 8;
        }
      encode_block(w, cb, qc, &pred_cb);
      encode_block(w, cr, qc, &pred_cr);
    }
  }
  finish_bits(w);
  jmarker(w, 0xd9);                // EOI
  if(jflush(w) < 0 || w->error)
    return -1;
  if(written)
    *written = w->total;
  return 0;
}

int
jpeg_encode(int fd, int width, int height, int quality,
            jpeg_pixel_fn pixel, void *arg, uint *written)
{
  struct jw w;
  if(fd < 0)
    return -1;
  memset(&w, 0, sizeof(w));
  w.fd = fd;
  return jpeg_encode_to(&w, width, height, quality, pixel, arg, written);
}

int
jpeg_encode_mem(uchar *dst, int cap, int width, int height, int quality,
                jpeg_pixel_fn pixel, void *arg, uint *written)
{
  struct jw w;
  if(dst == 0 || cap <= 0)
    return -1;
  memset(&w, 0, sizeof(w));
  w.fd = -1;
  w.mem = dst;
  w.cap = cap;
  return jpeg_encode_to(&w, width, height, quality, pixel, arg, written);
}
