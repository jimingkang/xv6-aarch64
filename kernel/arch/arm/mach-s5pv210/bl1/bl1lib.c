#include "kernel/types.h"
#include "bl1lib.h"

static uint32 crctab[256];
uint32 crc32(uint32 crc, const uchar *p, uint32 n) {
  if(!crctab[1]) {
    for(uint32 i=0;i<256;i++) {
      uint32 c=i;
      for(int k=0;k<8;k++) c = (c&1) ? 0xEDB88320U^(c>>1) : c>>1;
      crctab[i]=c;
    }
  }
  crc=~crc;
  while(n--) crc=crctab[(crc^*p++)&255]^(crc>>8);
  return ~crc;
}

static uint32 be32(const uchar *p) {
  return ((uint32)p[0]<<24)|((uint32)p[1]<<16)|((uint32)p[2]<<8)|p[3];
}

const char *uimage_check(const uchar *h, struct uimage_info *ui) {
  uchar copy[UIMAGE_HDR];
  if(be32(h)!=UIMAGE_MAGIC) return "no uImage magic";
  for(int i=0;i<UIMAGE_HDR;i++) copy[i]=h[i];
  copy[4]=copy[5]=copy[6]=copy[7]=0;
  if(crc32(0,copy,UIMAGE_HDR)!=be32(h+4)) return "bad header CRC";
  /* os=Linux(5) arch=ARM(2) type=kernel(2) comp=none(0) */
  if(h[28]!=5 || h[29]!=2 || h[30]!=2 || h[31]!=0) return "not an uncompressed ARM kernel";
  ui->size=be32(h+12); ui->load=be32(h+16); ui->entry=be32(h+20); ui->dcrc=be32(h+24);
  for(int i=0;i<32;i++) ui->name[i]=h[32+i];
  ui->name[32]=0;
  if(ui->size==0 || ui->size>(32U<<20)) return "bad image size";
  return 0;
}

uint32 atag_build(uint32 *t, uint32 mem_base, uint32 mem_size, const char *cmdline) {
  uint32 *s=t, n=0;
  t[0]=2; t[1]=0x54410001; t+=2;                          /* ATAG_CORE, no data */
  t[0]=4; t[1]=0x54410002; t[2]=mem_size; t[3]=mem_base; t+=4; /* ATAG_MEM */
  while(cmdline[n]) n++;
  uint32 words=(n+1+3)/4;
  t[0]=2+words; t[1]=0x54410009;                          /* ATAG_CMDLINE */
  char *d=(char*)(t+2);
  for(uint32 i=0;i<words*4;i++) d[i]= i<=n ? cmdline[i] : 0;
  t+=2+words;
  t[0]=0; t[1]=0; t+=2;                                    /* ATAG_NONE */
  return (uint32)((uchar*)t-(uchar*)s);
}
