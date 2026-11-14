#include "types.h"

void*
memset(void *dst, int c, uint n)
{
  // Word stores for the bulk: with the D-cache off every store goes to
  // DRAM, and byte-wise clearing of whole pages dominated boot time.
  uchar *d = (uchar *) dst;
  uchar b = c;
  while(n && ((uintptr)d & 3)){ *d++ = b; n--; }
  uint32 w = b | (uint32)b << 8 | (uint32)b << 16 | (uint32)b << 24;
  uint32 *wd = (uint32 *) d;
  for(; n >= 16; n -= 16, wd += 4){ wd[0] = w; wd[1] = w; wd[2] = w; wd[3] = w; }
  for(; n >= 4; n -= 4) *wd++ = w;
  d = (uchar *) wd;
  while(n--) *d++ = b;
  return dst;
}

int
memcmp(const void *v1, const void *v2, uint n)
{
  const uchar *s1, *s2;

  s1 = v1;
  s2 = v2;
  while(n-- > 0){
    if(*s1 != *s2)
      return *s1 - *s2;
    s1++, s2++;
  }

  return 0;
}

void*
memmove(void *dst, const void *src, uint n)
{
  const char *s;
  char *d;

  if(n == 0)
    return dst;

  s = src;
  d = dst;
  if(s < d && s + n > d){
    s += n;
    d += n;
    if((((uintptr)s | (uintptr)d) & 3) == 0){
      for(; n >= 4; n -= 4){ d -= 4; s -= 4; *(uint32*)d = *(const uint32*)s; }
    }
    while(n-- > 0)
      *--d = *--s;
  } else {
    if((((uintptr)s | (uintptr)d) & 3) == 0){
      for(; n >= 16; n -= 16, d += 16, s += 16){
        ((uint32*)d)[0] = ((const uint32*)s)[0]; ((uint32*)d)[1] = ((const uint32*)s)[1];
        ((uint32*)d)[2] = ((const uint32*)s)[2]; ((uint32*)d)[3] = ((const uint32*)s)[3];
      }
      for(; n >= 4; n -= 4, d += 4, s += 4) *(uint32*)d = *(const uint32*)s;
    }
    while(n-- > 0)
      *d++ = *s++;
  }

  return dst;
}

// memcpy exists to placate GCC.  Use memmove.
void*
memcpy(void *dst, const void *src, uint n)
{
  return memmove(dst, src, n);
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

char*
strncpy(char *s, const char *t, int n)
{
  char *os;

  os = s;
  while(n-- > 0 && (*s++ = *t++) != 0)
    ;
  while(n-- > 0)
    *s++ = 0;
  return os;
}

// Like strncpy but guaranteed to NUL-terminate.
char*
safestrcpy(char *s, const char *t, int n)
{
  char *os;

  os = s;
  if(n <= 0)
    return os;
  while(--n > 0 && (*s++ = *t++) != 0)
    ;
  *s = 0;
  return os;
}

int
strlen(const char *s)
{
  int n;

  for(n = 0; s[n]; n++)
    ;
  return n;
}

