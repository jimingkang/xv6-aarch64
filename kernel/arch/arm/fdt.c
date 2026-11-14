/* Minimal flattened-device-tree reader for the U-Boot handoff (r2).
   Runs before the MMU is on: all accesses are byte loads, so no
   alignment assumptions are made about the blob. Only reads:
     model, compatible, memory reg and chosen bootargs. */
#include "kernel/types.h"
#include "fdt.h"
#define FDT_MAGIC 0xd00dfeedU
#define FDT_BEGIN_NODE 1
#define FDT_END_NODE 2
#define FDT_PROP 3
#define FDT_NOP 4
#define FDT_END 9
static uint32 be32(const uchar *p) {
  return ((uint32)p[0]<<24)|((uint32)p[1]<<16)|((uint32)p[2]<<8)|p[3];
}
static int streq(const char *a, const char *b) {
  while(*a && *a==*b) { a++; b++; }
  return *a==*b;
}
static int prefix(const char *s, const char *p) {
  while(*p) if(*s++!=*p++) return 0;
  return 1;
}
static void copystr(char *dst, int max, const uchar *src, uint len) {
  int i=0;
  /* Multi-string properties (compatible) are shown separated by ", ". */
  for(uint k=0; k<len && i<max-1; k++) {
    char c=src[k];
    if(c==0) {
      if(k+1==len) break;
      if(i<max-2) { dst[i++]=','; dst[i++]=' '; }
      continue;
    }
    dst[i++]=c;
  }
  dst[i]=0;
}
static uint64 cells(const uchar *p, uint n) {
  return n==2 ? ((uint64)be32(p)<<32)|be32(p+4) : be32(p);
}
int fdt_parse(uintptr addr, struct fdt_info *info) {
  const uchar *h=(const uchar*)addr;
  info->valid=0; info->model[0]=0; info->compatible[0]=0; info->bootargs[0]=0;
  info->mem_base=info->mem_size=0;
  if(addr==0 || (addr&3)) return -1;
  if(be32(h)!=FDT_MAGIC) return -1;
  uint32 total=be32(h+4), offs=be32(h+8), offstr=be32(h+12), version=be32(h+20);
  if(total<40 || total>(1U<<20) || offs>=total || offstr>=total || version<16) return -1;
  info->size=total;
  const uchar *p=h+offs, *strs=h+offstr, *limit=h+total;
  uint acells=1, scells=1;
  int depth=0, in_mem=0, in_chosen=0;
  while(p+4<=limit) {
    uint32 tok=be32(p); p+=4;
    if(tok==FDT_BEGIN_NODE) {
      const char *name=(const char*)p;
      while(p<limit && *p) p++;
      p=(const uchar*)(((uintptr)p+4)&~3U);
      depth++;
      if(depth==2) {
        in_mem=streq(name,"memory") || prefix(name,"memory@");
        in_chosen=streq(name,"chosen");
      }
    } else if(tok==FDT_END_NODE) {
      if(depth==2) in_mem=in_chosen=0;
      depth--;
    } else if(tok==FDT_PROP) {
      if(p+8>limit) return -1;
      uint32 len=be32(p), nameoff=be32(p+4);
      const uchar *val=p+8;
      p=(const uchar*)(((uintptr)val+len+3)&~3U);
      if(p>limit || offstr+nameoff>=total) return -1;
      const char *pn=(const char*)strs+nameoff;
      if(depth==1) {
        if(streq(pn,"#address-cells") && len==4) acells=be32(val);
        else if(streq(pn,"#size-cells") && len==4) scells=be32(val);
        else if(streq(pn,"model")) copystr(info->model,sizeof(info->model),val,len);
        else if(streq(pn,"compatible")) copystr(info->compatible,sizeof(info->compatible),val,len);
      } else if(depth==2 && in_mem && streq(pn,"reg") && !info->mem_size) {
        if(acells<1 || acells>2 || scells<1 || scells>2 || len<4*(acells+scells)) continue;
        info->mem_base=cells(val,acells);
        info->mem_size=cells(val+4*acells,scells);
      } else if(depth==2 && in_chosen && streq(pn,"bootargs")) {
        copystr(info->bootargs,sizeof(info->bootargs),val,len);
      }
    } else if(tok==FDT_NOP) {
    } else if(tok==FDT_END) {
      info->valid=1;
      return 0;
    } else return -1;
  }
  return -1;
}
