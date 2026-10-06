#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#include <stdarg.h>

static char digits[] = "0123456789ABCDEF";

static int
putc(int fd, char c)
{
  return write(fd, &c, 1) == 1 ? 1 : 0;
}

static int
printint(int fd, int xx, int base, int sgn)
{
  char buf[16];
  int i, neg, written = 0;
  uint x;

  neg = 0;
  if(sgn && xx < 0){
    neg = 1;
    x = -xx;
  } else {
    x = xx;
  }

  i = 0;
  do{
    buf[i++] = digits[x % base];
  }while((x /= base) != 0);
  if(neg)
    buf[i++] = '-';

  while(--i >= 0)
    written += putc(fd, buf[i]);
  return written;
}

static int
printuint64(int fd, uint64 x, int base)
{
  char buf[32];
  int i = 0, written = 0;

  do{
    buf[i++] = digits[x % base];
  }while((x /= base) != 0);
  while(--i >= 0)
    written += putc(fd, buf[i]);
  return written;
}

static int
printptr(int fd, uint64 x) {
  int i, written = 0;
  written += putc(fd, '0');
  written += putc(fd, 'x');
  for (i = 0; i < (sizeof(uint64) * 2); i++, x <<= 4)
    written += putc(fd, digits[x >> (sizeof(uint64) * 8 - 4)]);
  return written;
}

// Print to the given fd. Only understands %d, %x, %p, %s.
static int
vprintf(int fd, const char *fmt, va_list ap)
{
  char *s;
  int c, i, state, written = 0;

  state = 0;
  for(i = 0; fmt[i]; i++){
    c = fmt[i] & 0xff;
    if(state == 0){
      if(c == '%'){
        state = '%';
      } else {
        written += putc(fd, c);
      }
    } else if(state == '%'){
      if(c == 'd'){
        written += printint(fd, va_arg(ap, int), 10, 1);
      } else if(c == 'l') {
        written += printuint64(fd, va_arg(ap, uint64), 10);
      } else if(c == 'x') {
        written += printint(fd, va_arg(ap, int), 16, 0);
      } else if(c == 'p') {
        written += printptr(fd, va_arg(ap, uint64));
      } else if(c == 's'){
        s = va_arg(ap, char*);
        if(s == 0)
          s = "(null)";
        while(*s != 0){
          written += putc(fd, *s);
          s++;
        }
      } else if(c == 'c'){
        written += putc(fd, va_arg(ap, uint));
      } else if(c == '%'){
        written += putc(fd, c);
      } else {
        // Unknown % sequence.  Print it to draw attention.
        written += putc(fd, '%');
        written += putc(fd, c);
      }
      state = 0;
    }
  }
  return written;
}

int
fprintf(int fd, const char *fmt, ...)
{
  va_list ap;
  int result;

  va_start(ap, fmt);
  result = vprintf(fd, fmt, ap);
  va_end(ap);
  return result;
}

int
printf(const char *fmt, ...)
{
  va_list ap;
  int result;

  va_start(ap, fmt);
  result = vprintf(1, fmt, ap);
  va_end(ap);
  return result;
}
