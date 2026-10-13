#include <uapi/types.h>
#include <uapi/stat.h>
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
vfdprintf(int fd, const char *fmt, va_list ap)
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
  result = vfdprintf(fd, fmt, ap);
  va_end(ap);
  return result;
}

int
printf(const char *fmt, ...)
{
  va_list ap;
  int result;

  va_start(ap, fmt);
  result = vfdprintf(1, fmt, ap);
  va_end(ap);
  return result;
}

int
vfprintf(int fd, const char *fmt, va_list ap)
{
  return vfdprintf(fd, fmt, ap);
}

int
vprintf(const char *fmt, va_list ap)
{
  return vfdprintf(1, fmt, ap);
}

static void
bufchar(char *buffer, uint size, int *total, char c)
{
  if(size > 0 && (uint)*total < size - 1)
    buffer[*total] = c;
  (*total)++;
}

static void
bufuint(char *buffer, uint size, int *total, uint64 value, int base,
        int negative)
{
  char temporary[32];
  int n = 0;
  do {
    temporary[n++] = digits[value % base];
    value /= base;
  } while(value);
  if(negative)
    temporary[n++] = '-';
  while(n)
    bufchar(buffer, size, total, temporary[--n]);
}

int
vsnprintf(char *buffer, uint size, const char *fmt, va_list ap)
{
  int total = 0;
  if(buffer == 0 && size != 0)
    return -1;
  for(int i = 0; fmt[i]; i++){
    int c = fmt[i] & 0xff;
    if(c != '%'){
      bufchar(buffer, size, &total, c);
      continue;
    }
    c = fmt[++i] & 0xff;
    if(c == 0)
      break;
    if(c == '%'){
      bufchar(buffer, size, &total, '%');
    } else if(c == 'c'){
      bufchar(buffer, size, &total, va_arg(ap, int));
    } else if(c == 's'){
      char *s = va_arg(ap, char*);
      if(s == 0)
        s = "(null)";
      while(*s)
        bufchar(buffer, size, &total, *s++);
    } else if(c == 'd' || c == 'i'){
      int value = va_arg(ap, int);
      uint magnitude = value < 0 ? -(uint)value : (uint)value;
      bufuint(buffer, size, &total, magnitude, 10, value < 0);
    } else if(c == 'u'){
      bufuint(buffer, size, &total, va_arg(ap, uint), 10, 0);
    } else if(c == 'x'){
      bufuint(buffer, size, &total, va_arg(ap, uint), 16, 0);
    } else if(c == 'p'){
      uint64 value = va_arg(ap, uint64);
      bufchar(buffer, size, &total, '0');
      bufchar(buffer, size, &total, 'x');
      bufuint(buffer, size, &total, value, 16, 0);
    } else if(c == 'l'){
      int signed_value = fmt[i + 1] == 'd' || fmt[i + 1] == 'i';
      int base = fmt[i + 1] == 'x' ? 16 : 10;
      uint64 value;
      int negative = 0;
      if(fmt[i + 1] == 'd' || fmt[i + 1] == 'i' ||
         fmt[i + 1] == 'u' || fmt[i + 1] == 'x')
        i++;
      if(signed_value){
        long signed_long = va_arg(ap, long);
        negative = signed_long < 0;
        value = negative ? -(uint64)signed_long : (uint64)signed_long;
      } else {
        value = va_arg(ap, uint64);
      }
      bufuint(buffer, size, &total, value, base, negative);
    } else {
      bufchar(buffer, size, &total, '%');
      bufchar(buffer, size, &total, c);
    }
  }
  if(size > 0)
    buffer[(uint)total < size ? total : size - 1] = 0;
  return total;
}

int
snprintf(char *buffer, uint size, const char *fmt, ...)
{
  va_list ap;
  int result;
  va_start(ap, fmt);
  result = vsnprintf(buffer, size, fmt, ap);
  va_end(ap);
  return result;
}

int
sprintf(char *buffer, const char *fmt, ...)
{
  va_list ap;
  int result;
  va_start(ap, fmt);
  result = vsnprintf(buffer, (uint)-1, fmt, ap);
  va_end(ap);
  return result;
}

int
getchar(void)
{
  uchar c;
  return read(0, &c, 1) == 1 ? c : -1;
}
