/* ARM Linux tagged list (Documentation/arch/arm/booting.rst, setup.h).
   The X210 U-Boot (do_bootm_linux) writes ATAG_CORE, one ATAG_MEM per DRAM
   bank, optional ATAG_SERIAL/REVISION/INITRD2, ATAG_CMDLINE and ATAG_NONE
   at bi_boot_params = 0x30000100. Read before the MMU and allocator run. */
#include "kernel/types.h"
#include "atag.h"
#define ATAG_NONE    0x00000000U
#define ATAG_CORE    0x54410001U
#define ATAG_MEM     0x54410002U
#define ATAG_SERIAL  0x54410006U
#define ATAG_REVISION 0x54410007U
#define ATAG_INITRD2 0x54420005U
#define ATAG_CMDLINE 0x54410009U
#define ATAG_LIMIT   4096U       /* sanity bound on the list length */

int atag_parse(uintptr addr, struct bootparams *bp) {
  const uint32 *t=(const uint32*)addr, *limit=(const uint32*)(addr+ATAG_LIMIT);
  if(!addr || (addr&3)) return -1;
  /* ATAG_CORE must come first; its size is 2 (no data) or 5 words. */
  if(t[1]!=ATAG_CORE || (t[0]!=2 && t[0]!=5)) return -1;
  bp->nmem=0; bp->cmdline[0]=0; bp->initrd_base=bp->initrd_size=0;
  bp->serial_lo=bp->serial_hi=bp->revision=0;
  while(t+2<=limit) {
    uint32 size=t[0], tag=t[1];
    if(tag==ATAG_NONE) { bp->kind=1; bp->where=addr; return 0; }
    if(size<2 || t+size>limit) return -1;
    const uint32 *d=t+2;
    if(tag==ATAG_MEM && size>=4 && bp->nmem<BOOTP_MAXMEM) {
      bp->mem_size[bp->nmem]=d[0]; bp->mem_base[bp->nmem]=d[1]; bp->nmem++;
    } else if(tag==ATAG_CMDLINE) {
      const char *s=(const char*)d; uint max=(size-2)*4, i=0;
      while(i<max && i<sizeof(bp->cmdline)-1 && s[i]) { bp->cmdline[i]=s[i]; i++; }
      bp->cmdline[i]=0;
    } else if(tag==ATAG_INITRD2 && size>=4) {
      bp->initrd_base=d[0]; bp->initrd_size=d[1];
    } else if(tag==ATAG_SERIAL && size>=4) {
      bp->serial_lo=d[0]; bp->serial_hi=d[1];
    } else if(tag==ATAG_REVISION && size>=3) {
      bp->revision=d[0];
    }
    t+=size;
  }
  return -1;
}
