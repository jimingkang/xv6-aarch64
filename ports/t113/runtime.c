/* Freestanding compiler helpers; no host libc enters the target image. */
#include "kernel/types.h"
#include "kernel/aarch64.h"
#include "kernel/defs.h"
void __aeabi_memcpy(void *d,const void *s,uint n) { memmove(d,s,n); }
void __aeabi_memcpy4(void *d,const void *s,uint n) { memmove(d,s,n); }
void __aeabi_memcpy8(void *d,const void *s,uint n) { memmove(d,s,n); }
void __aeabi_memclr(void *d,uint n) { memset(d,0,n); }
void __aeabi_memclr4(void *d,uint n) { memset(d,0,n); }
void __aeabi_memclr8(void *d,uint n) { memset(d,0,n); }
void __aeabi_memset(void *d,uint n,int c) { memset(d,c,n); }
uint64 t113_udiv(uint64 a,uint64 b,uint64 *rem) {
  uint64 q=0,r=0;
  if(!b) { *rem=a; return 0; }
  for(int i=63;i>=0;i--) {
    uint carry=r>>63;
    r=(r<<1)|((a>>i)&1);
    if(carry || r>=b) { r-=b; q|=1ULL<<i; }
  }
  *rem=r; return q;
}
