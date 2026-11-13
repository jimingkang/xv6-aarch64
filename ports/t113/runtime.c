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
#ifndef __ARM_FEATURE_IDIV
/* Cortex-A8 (AM335x) has no UDIV/SDIV: provide the EABI 32-bit helpers.
   The divmod variants return quotient in r0 and remainder in r1. */
static uint udiv32(uint a,uint b,uint *rem) {
  uint q=0,r=0;
  if(!b) { *rem=a; return 0; }
  for(int i=31;i>=0;i--) {
    r=(r<<1)|((a>>i)&1);
    if(r>=b) { r-=b; q|=1U<<i; }
  }
  *rem=r; return q;
}
uint __aeabi_uidiv(uint a,uint b) { uint r; return udiv32(a,b,&r); }
uint64 __aeabi_uidivmod(uint a,uint b) { uint r, q=udiv32(a,b,&r); return ((uint64)r<<32)|q; }
int __aeabi_idiv(int a,int b) {
  uint r, q=udiv32(a<0?0U-(uint)a:(uint)a, b<0?0U-(uint)b:(uint)b, &r);
  return (a<0)!=(b<0) ? -(int)q : (int)q;
}
uint64 __aeabi_idivmod(int a,int b) {
  uint r, q=udiv32(a<0?0U-(uint)a:(uint)a, b<0?0U-(uint)b:(uint)b, &r);
  int qs=(a<0)!=(b<0) ? -(int)q : (int)q, rs=a<0 ? -(int)r : (int)r;
  return ((uint64)(uint)rs<<32)|(uint)qs;
}
#endif
