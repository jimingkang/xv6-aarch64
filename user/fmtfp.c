// fmtfp.c -- %e/%f/%g formatting of doubles for printf and the C++ ostream.
//
// Adapted from musl 1.2.5 src/stdio/vfprintf.c (fmt_fp, MIT licence, see
// user/libm/COPYRIGHT.musl).  musl works on long double and turns the value
// into base-1e9 digits with floating multiplies, which is exact for its
// 113-bit long double.  Here the 53-bit mantissa is taken as an integer, so
// the conversion stays exact with plain double and no soft-float long double
// (libgcc's __multf3 etc.) is needed.  Output goes to a bounded buffer.
#include <uapi/types.h>
#include <float.h>
#include "user/user.h"
#include "user/fmtfp.h"

typedef unsigned int u32;

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define INT_MAX 0x7fffffff

struct sink {
  char *buf;
  int cap;                    // bytes available, including the final NUL
  int len;                    // bytes produced (may exceed cap)
};

static void
out(struct sink *f, const char *s, int n)
{
  for(int i = 0; i < n; i++, f->len++)
    if(f->len < f->cap - 1)
      f->buf[f->len] = s[i];
}

static void
pad(struct sink *f, char c, int w, int l, int fl)
{
  char p[64];
  if(fl & (FMT_LEFT | FMT_ZERO) || l >= w)
    return;
  l = w - l;
  memset(p, c, l > (int)sizeof(p) ? (int)sizeof(p) : l);
  for(; l >= (int)sizeof(p); l -= sizeof(p))
    out(f, p, sizeof(p));
  out(f, p, l);
}

static char *
fmt_u(unsigned long x, char *s)
{
  for(; x; x /= 10)
    *--s = '0' + x % 10;
  return s;
}

static int
fmt_fp(struct sink *f, double y, int w, int p, int fl, int t)
{
  // Mantissa expansion plus exponent expansion, as in musl.
  u32 big[(DBL_MANT_DIG + 28) / 29 + 1 + (DBL_MAX_EXP + DBL_MANT_DIG + 28 + 8) / 9 + 2];
  u32 *a, *d, *r, *z;
  int e2 = 0, e, i, j, l;
  char buf[9 + DBL_MANT_DIG / 4], *s;
  const char *prefix = "-0X+0X 0X-0x+0x 0x";
  int pl;
  char ebuf0[3 * sizeof(int)], *ebuf = &ebuf0[3 * sizeof(int)], *estr = 0;
  union { double f; unsigned long i; } u = { y };

  pl = 1;
  if(u.i >> 63){
    y = -y;
    u.f = y;
  } else if(fl & FMT_PLUS){
    prefix += 3;
  } else if(fl & FMT_SPACE){
    prefix += 6;
  } else {
    prefix++;
    pl = 0;
  }

  if(((u.i >> 52) & 0x7ff) == 0x7ff){
    const char *str = (t & 32) ? "inf" : "INF";
    if(u.i << 12)
      str = (t & 32) ? "nan" : "NAN";
    pad(f, ' ', w, 3 + pl, fl & ~FMT_ZERO);
    out(f, prefix, pl);
    out(f, str, 3);
    pad(f, ' ', w, 3 + pl, fl ^ FMT_LEFT);
    return MAX(w, 3 + pl);
  }

  // y = m * 2^e2 with m an integer below 2^53.
  unsigned long m;
  {
    int ex = (u.i >> 52) & 0x7ff;
    m = u.i & ((1UL << 52) - 1);
    if(ex)
      m |= 1UL << 52;
    else
      ex = 1;
    e2 = ex - 1075;
  }

  if(p < 0)
    p = 6;

  if(e2 < 0){
    a = big;
    r = big + 1;
  } else {
    r = big + sizeof(big) / sizeof(*big) - DBL_MANT_DIG - 1;
    a = r - 1;
  }
  a[0] = m / 1000000000;
  r[0] = m % 1000000000;
  z = r + 1;
  if(a[0] == 0)
    a = r;
  if(m == 0){
    a = r;
    z = r;
    e2 = 0;
  }

  while(e2 > 0){
    u32 carry = 0;
    int sh = MIN(29, e2);
    for(d = z - 1; d >= a; d--){
      unsigned long x = ((unsigned long)*d << sh) + carry;
      *d = x % 1000000000;
      carry = x / 1000000000;
    }
    if(carry)
      *--a = carry;
    while(z > a && !z[-1])
      z--;
    e2 -= sh;
  }
  while(e2 < 0){
    u32 carry = 0, *b;
    int sh = MIN(9, -e2), need = 1 + (p + DBL_MANT_DIG / 3U + 8) / 9;
    for(d = a; d < z; d++){
      u32 rm = *d & ((1 << sh) - 1);
      *d = (*d >> sh) + carry;
      carry = (1000000000 >> sh) * rm;
    }
    if(!*a)
      a++;
    if(carry)
      *z++ = carry;
    // Avoid computation past the requested precision.
    b = (t | 32) == 'f' ? r : a;
    if(z - b > need)
      z = b + need;
    e2 += sh;
  }

  if(a < z)
    for(i = 10, e = 9 * (r - a); *a >= (u32)i; i *= 10, e++)
      ;
  else
    e = 0;

  // Round: j is the precision after the radix point (possibly negative).
  j = p - ((t | 32) != 'f') * e - ((t | 32) == 'g' && p);
  if(j < 9 * (z - r - 1)){
    u32 x;
    d = r + 1 + ((j + 9 * DBL_MAX_EXP) / 9 - DBL_MAX_EXP);
    j += 9 * DBL_MAX_EXP;
    j %= 9;
    for(i = 10, j++; j < 9; i *= 10, j++)
      ;
    x = *d % i;
    if(x || d + 1 != z){
      double round = 2 / DBL_EPSILON;
      double small;
      if((*d / i & 1) || (i == 1000000000 && d > a && (d[-1] & 1)))
        round += 2;
      if(x < (u32)i / 2)
        small = 0x0.8p0;
      else if(x == (u32)i / 2 && d + 1 == z)
        small = 0x1.0p0;
      else
        small = 0x1.8p0;
      if(pl && *prefix == '-'){
        round *= -1;
        small *= -1;
      }
      *d -= x;
      if(round + small != round){
        *d = *d + i;
        while(*d > 999999999){
          *d-- = 0;
          if(d < a)
            *--a = 0;
          (*d)++;
        }
        for(i = 10, e = 9 * (r - a); *a >= (u32)i; i *= 10, e++)
          ;
      }
    }
    if(z > d + 1)
      z = d + 1;
  }
  for(; z > a && !z[-1]; z--)
    ;

  if((t | 32) == 'g'){
    if(!p)
      p++;
    if(p > e && e >= -4){
      t--;
      p -= e + 1;
    } else {
      t -= 2;
      p--;
    }
    if(!(fl & FMT_ALT)){
      if(z > a && z[-1])
        for(i = 10, j = 0; z[-1] % i == 0; i *= 10, j++)
          ;
      else
        j = 9;
      if((t | 32) == 'f')
        p = MIN(p, MAX(0, 9 * (z - r - 1) - j));
      else
        p = MIN(p, MAX(0, 9 * (z - r - 1) + e - j));
    }
  }
  l = 1 + p + (p || (fl & FMT_ALT));
  if((t | 32) == 'f'){
    if(e > 0)
      l += e;
  } else {
    estr = fmt_u(e < 0 ? -e : e, ebuf);
    while(ebuf - estr < 2)
      *--estr = '0';
    *--estr = (e < 0 ? '-' : '+');
    *--estr = t;
    l += ebuf - estr;
  }

  pad(f, ' ', w, pl + l, fl);
  out(f, prefix, pl);
  pad(f, '0', w, pl + l, fl ^ FMT_ZERO);

  if((t | 32) == 'f'){
    if(a > r)
      a = r;
    for(d = a; d <= r; d++){
      s = fmt_u(*d, buf + 9);
      if(d != a)
        while(s > buf)
          *--s = '0';
      else if(s == buf + 9)
        *--s = '0';
      out(f, s, buf + 9 - s);
    }
    if(p || (fl & FMT_ALT))
      out(f, ".", 1);
    for(; d < z && p > 0; d++, p -= 9){
      s = fmt_u(*d, buf + 9);
      while(s > buf)
        *--s = '0';
      out(f, s, MIN(9, p));
    }
    pad(f, '0', p + 9, 9, 0);
  } else {
    if(z <= a)
      z = a + 1;
    for(d = a; d < z && p >= 0; d++){
      s = fmt_u(*d, buf + 9);
      if(s == buf + 9)
        *--s = '0';
      if(d != a)
        while(s > buf)
          *--s = '0';
      else {
        out(f, s++, 1);
        if(p > 0 || (fl & FMT_ALT))
          out(f, ".", 1);
      }
      out(f, s, MIN(buf + 9 - s, p));
      p -= buf + 9 - s;
    }
    pad(f, '0', p + 18, 18, 0);
    out(f, estr, ebuf - estr);
  }

  pad(f, ' ', w, pl + l, fl ^ FMT_LEFT);
  return MAX(w, pl + l);
}

int
fmt_double(char *buf, int cap, double value, int width, int prec, int flags,
           int conv)
{
  struct sink f = { buf, cap, 0 };
  if(conv != 'e' && conv != 'E' && conv != 'f' && conv != 'F' &&
     conv != 'g' && conv != 'G')
    conv = 'g';
  if(conv == 'F')
    conv = 'f';
  fmt_fp(&f, value, width, prec, flags, conv);
  if(cap > 0)
    buf[f.len < cap ? f.len : cap - 1] = 0;
  return f.len;
}
