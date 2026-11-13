/* Boot parameters handed over by U-Boot in r2: ATAG list or DTB. */
#define BOOTP_MAXMEM 4
struct bootparams {
  int kind;                 /* 0 none, 1 ATAGs, 2 DTB */
  uint32 where;             /* address the list was found at */
  int nmem;
  uint32 mem_base[BOOTP_MAXMEM], mem_size[BOOTP_MAXMEM];
  uint32 initrd_base, initrd_size;
  uint32 serial_lo, serial_hi, revision;
  char cmdline[128];
  char model[64];           /* DTB only */
};
int atag_parse(uintptr addr, struct bootparams *bp);
