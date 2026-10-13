#include <uapi/types.h>
#include <uapi/stat.h>
#include <fcntl.h>
#include "user/user.h"

char*
strcpy(char *s, const char *t)
{
  char *os;

  os = s;
  while((*s++ = *t++) != 0)
    ;
  return os;
}

char*
strncpy(char *s, const char *t, int n)
{
  char *os = s;
  while(n > 0 && *t){
    *s++ = *t++;
    n--;
  }
  while(n-- > 0)
    *s++ = 0;
  return os;
}

char*
strcat(char *s, const char *t)
{
  char *os = s;
  while(*s)
    s++;
  while((*s++ = *t++) != 0)
    ;
  return os;
}

int
strcmp(const char *p, const char *q)
{
  while(*p && *p == *q)
    p++, q++;
  return (uchar)*p - (uchar)*q;
}

int
strncmp(const char *p, const char *q, uint n)
{
  while(n > 0 && *p && *p == *q)
    n--, p++, q++;
  if(n == 0)
    return 0;
  return (uchar)*p - (uchar)*q;
}

uint
strlen(const char *s)
{
  int n;

  for(n = 0; s[n]; n++)
    ;
  return n;
}

uint
strnlen(const char *s, uint max)
{
  uint n = 0;
  while(n < max && s[n])
    n++;
  return n;
}

void*
memset(void *dst, int c, uint n)
{
  char *cdst = (char *) dst;
  int i;
  for(i = 0; i < n; i++){
    cdst[i] = c;
  }
  return dst;
}

char*
strchr(const char *s, char c)
{
  for(; *s; s++)
    if(*s == c)
      return (char*)s;
  return c == 0 ? (char*)s : 0;
}

char*
strrchr(const char *s, int c)
{
  const char *last = 0;
  do {
    if((uchar)*s == (uchar)c)
      last = s;
  } while(*s++);
  return (char*)last;
}

char*
strncat(char *dst, const char *src, uint n)
{
  char *result = dst;
  while(*dst)
    dst++;
  while(n-- && *src)
    *dst++ = *src++;
  *dst = 0;
  return result;
}

char*
strstr(const char *haystack, const char *needle)
{
  uint n = strlen(needle);
  if(n == 0)
    return (char*)haystack;
  for(; *haystack; haystack++)
    if(*haystack == *needle && strncmp(haystack, needle, n) == 0)
      return (char*)haystack;
  return 0;
}

uint
strspn(const char *s, const char *accept)
{
  uint n = 0;
  while(s[n] && strchr(accept, s[n]))
    n++;
  return n;
}

uint
strcspn(const char *s, const char *reject)
{
  uint n = 0;
  while(s[n] && !strchr(reject, s[n]))
    n++;
  return n;
}

char*
strpbrk(const char *s, const char *accept)
{
  for(; *s; s++)
    if(strchr(accept, *s))
      return (char*)s;
  return 0;
}

void*
memchr(const void *memory, int c, uint n)
{
  const uchar *p = memory;
  while(n--)
    if(*p++ == (uchar)c)
      return (void*)(p - 1);
  return 0;
}

char*
strdup(const char *s)
{
  uint n = strlen(s) + 1;
  char *copy = malloc(n);
  if(copy)
    memmove(copy, s, n);
  return copy;
}

char*
strtok(char *s, const char *delimiters)
{
  static char *next;
  char *token;
  if(s)
    next = s;
  if(next == 0)
    return 0;
  next += strspn(next, delimiters);
  if(*next == 0){
    next = 0;
    return 0;
  }
  token = next;
  next += strcspn(next, delimiters);
  if(*next)
    *next++ = 0;
  else
    next = 0;
  return token;
}

char*
gets(char *buf, int max)
{
  int i, cc;
  char c;

  for(i=0; i+1 < max; ){
    cc = read(0, &c, 1);
    if(cc < 1)
      break;
    buf[i++] = c;
    if(c == '\n' || c == '\r')
      break;
  }
  buf[i] = '\0';
  return buf;
}

int
stat(const char *n, struct stat *st)
{
  int fd;
  int r;

  fd = open(n, O_RDONLY);
  if(fd < 0)
    return -1;
  r = fstat(fd, st);
  close(fd);
  return r;
}

int
atoi(const char *s)
{
  int n;

  n = 0;
  while('0' <= *s && *s <= '9')
    n = n*10 + *s++ - '0';
  return n;
}

long
strtol(const char *s, char **end, int base)
{
  const char *start;
  unsigned long value = 0;
  int negative = 0, digit;
  while(*s == ' ' || *s == '\t' || *s == '\n')
    s++;
  if(*s == '+' || *s == '-')
    negative = *s++ == '-';
  if((base == 0 || base == 16) && s[0] == '0' &&
     (s[1] == 'x' || s[1] == 'X')){
    base = 16;
    s += 2;
  } else if(base == 0) {
    base = *s == '0' ? 8 : 10;
  }
  start = s;
  while(*s){
    if(*s >= '0' && *s <= '9')
      digit = *s - '0';
    else if(*s >= 'a' && *s <= 'z')
      digit = *s - 'a' + 10;
    else if(*s >= 'A' && *s <= 'Z')
      digit = *s - 'A' + 10;
    else
      break;
    if(digit >= base)
      break;
    value = value * base + digit;
    s++;
  }
  if(end)
    *end = (char*)(s == start ? start : s);
  return negative ? -(long)value : (long)value;
}

unsigned long
strtoul(const char *s, char **end, int base)
{
  return (unsigned long)strtol(s, end, base);
}

int abs(int value) { return value < 0 ? -value : value; }
long labs(long value) { return value < 0 ? -value : value; }

void*
memmove(void *vdst, const void *vsrc, int n)
{
  char *dst;
  const char *src;

  dst = vdst;
  src = vsrc;
  if (src > dst) {
    while(n-- > 0)
      *dst++ = *src++;
  } else {
    dst += n;
    src += n;
    while(n-- > 0)
      *--dst = *--src;
  }
  return vdst;
}

int
memcmp(const void *s1, const void *s2, uint n)
{
  const char *p1 = s1, *p2 = s2;
  while (n-- > 0) {
    if (*p1 != *p2) {
      return *p1 - *p2;
    }
    p1++;
    p2++;
  }
  return 0;
}

void *
memcpy(void *dst, const void *src, uint n)
{
  return memmove(dst, src, n);
}

int isdigit(int c) { return c >= '0' && c <= '9'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int isalpha(int c) { return islower(c) || isupper(c); }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isascii(int c) { return (uint)c <= 0x7f; }
int isblank(int c) { return c == ' ' || c == '\t'; }
int iscntrl(int c) { return (uint)c < 0x20 || c == 0x7f; }
int isgraph(int c) { return c > ' ' && c <= '~'; }
int isprint(int c) { return c >= ' ' && c <= '~'; }
int ispunct(int c) { return isgraph(c) && !isalnum(c); }
int isspace(int c)
{ return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
int isxdigit(int c)
{ return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int tolower(int c) { return isupper(c) ? c - 'A' + 'a' : c; }
int toupper(int c) { return islower(c) ? c - 'a' + 'A' : c; }
