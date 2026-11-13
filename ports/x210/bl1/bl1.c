/* xv6 BL1 for the 九鼎 X210 (Samsung S5PV210, Cortex-A8): boots xv6 with
   no U-Boot in between.

     iROM (BL0) ──► this BL1 (SD sector 1, iRAM 0xD0020010)
                ──► xv6 uImage (SD sector 49) copied to DDR 0x30007FC0,
                    so its payload sits at 0x30008000
                ──► jump with r0=0, r1=2456 (SMDKV210), r2=ATAGs @ 0x30000100

   The board bring-up mirrors what the X210 BSP U-Boot does in
   lowlevel_init/cpu_init.S (same register values):
     PS_HOLD power latch, watchdog off, APLL 1000 MHz / MPLL 667 MHz with
     MSYS 200/100, DSYS 166/83, PSYS 133/66.7 MHz, UART2 115200 8N1,
     DMC0 DDR2 (2 x 1 Gbit x16 = 256 MiB at 0x30000000).
   DMC1 (the other 256 MiB at 0x40000000) is left uninitialised: xv6 only
   uses the first 128 MiB, and ATAG_MEM reports DMC0 only.
   The SD copy uses the iROM's CopySDMMCtoMem (pointer at 0xD0037F98),
   reading from the channel the iROM booted from (0xD0037488). */
#include "kernel/types.h"
#include "bl1lib.h"

#define REG(a) (*(volatile uint32*)(uintptr)(a))

/* Clock controller */
#define CLK 0xE0100000U
#define APLL_LOCK (CLK+0x000)
#define MPLL_LOCK (CLK+0x008)
#define APLL_CON0 (CLK+0x100)
#define MPLL_CON  (CLK+0x108)
#define CLK_SRC0  (CLK+0x200)
#define CLK_DIV0  (CLK+0x300)
#define CLK_DIV0_VAL 0x14131440U  /* A2M/5, HCLK_MSYS/5, PCLK_MSYS/2, HCLK_DSYS/4,
                                     PCLK_DSYS/2, HCLK_PSYS/5, PCLK_PSYS/2 */
#define PLL_EN (1U<<31)
#define PLL_LOCKED (1U<<29)
#define PS_HOLD_CONTROL 0xE010E81CU
/* GPIO */
#define GPIO 0xE0200000U
#define GPA1CON (GPIO+0x020)
#define MP1_DRV(n) (GPIO+0x3CC+0x20*(n))   /* DMC0 pad drive strength */
/* UART2 */
#define UART 0xE2900800U
#define ULCON 0x00
#define UCON 0x04
#define UFCON 0x08
#define UMCON 0x0C
#define UTRSTAT 0x10
#define UTXH 0x20
#define UBRDIV 0x28
#define UDIVSLOT 0x2C
#define UINTM 0x38
/* Watchdog */
#define WTCON 0xE2700000U
/* DMC0 */
#define DMC0 0xF0000000U
#define CONCONTROL 0x00
#define MEMCONTROL 0x04
#define MEMCONFIG0 0x08
#define MEMCONFIG1 0x0C
#define DIRECTCMD 0x10
#define PRECHCONFIG 0x14
#define PHYCONTROL0 0x18
#define PHYCONTROL1 0x1C
#define PWRDNCONFIG 0x28
#define TIMINGAREF 0x30
#define TIMINGROW 0x34
#define TIMINGDATA 0x38
#define TIMINGPOWER 0x3C
#define PHYSTATUS 0x40
/* iROM services (S5PV210 iROM application note) */
#define IROM_SDMMC_BASE 0xD0037488U   /* controller the iROM booted from */
#define IROM_COPY_SDMMC 0xD0037F98U   /* CopySDMMCtoMem pointer */
typedef uint32 (*copy_sdmmc_fn)(uint32 ch, uint32 start, uint16 count, uint32 *dst, uint32 init);

/* Layout shared with mkx210.py. */
#define DRAM_BASE 0x30000000U
#define DRAM_SIZE (256U<<20)          /* DMC0 */
#define KERNEL_SECTOR 49U
#define KERNEL_RUN 0x30008000U
#define ATAG_ADDR (DRAM_BASE+0x100)
#define MACH_SMDKV210 2456U
#define CMDLINE "console=ttySAC2,115200 xv6 bl1"

void bl1_boot(uint32 entry, uint32 machid, uint32 atags) __attribute__((noreturn));

static void uputc(int c) {
  while(!(REG(UART+UTRSTAT)&4)) ;     /* transmitter empty */
  REG(UART+UTXH)=(uchar)c;
}
static void uputs(const char *s) { while(*s) { if(*s=='\n') uputc('\r'); uputc(*s++); } }
static void puthex(uint32 v) {
  uputs("0x");
  for(int i=28;i>=0;i-=4) uputc("0123456789abcdef"[(v>>i)&15]);
}
static void putdec(uint32 v) {
  char b[11]; int i=10; b[i]=0;
  do {                                 /* v/10 without a divide instruction */
    uint32 q=(uint32)(((uint64)v*0xCCCCCCCDULL)>>35);
    b[--i]=(char)('0'+(v-q*10)); v=q;
  } while(v);
  uputs(b+i);
}
static void die(const char *why) {
  uputs("BL1: "); uputs(why); uputs(", halted\n");
  for(;;) asm volatile("wfi");
}

static void power_hold(void) {
  /* X210 has a soft power key: latch PS_HOLD high or the board turns off
     when the key is released. */
  REG(PS_HOLD_CONTROL)|=0x301;
  REG(WTCON)=0;
}

static void clock_init(void) {
  REG(CLK_SRC0)=0;                    /* everything from XXTI 24 MHz meanwhile */
  REG(APLL_LOCK)=0xFFFF;
  REG(MPLL_LOCK)=0xFFFF;
  REG(CLK_DIV0)=CLK_DIV0_VAL;
  REG(APLL_CON0)=PLL_EN|(125U<<16)|(3U<<8)|1;   /* 125*24/(3*2^0) = 1000 MHz */
  REG(MPLL_CON)=PLL_EN|(667U<<16)|(12U<<8)|1;   /* 667*24/(12*2^1) = 667 MHz */
  while(!(REG(APLL_CON0)&PLL_LOCKED)) ;
  while(!(REG(MPLL_CON)&PLL_LOCKED)) ;
  /* APLL_SEL, MPLL_SEL = FOUT; MSYS from APLL, DSYS/PSYS from MPLL. */
  REG(CLK_SRC0)=0x00000011;
}

static void uart_init(void) {
  REG(GPA1CON)=(REG(GPA1CON)&~0xffU)|0x22;      /* GPA1[0:1] = UART2 RXD/TXD */
  REG(UART+UFCON)=0;
  REG(UART+UMCON)=0;
  REG(UART+ULCON)=3;                            /* 8N1 */
  REG(UART+UCON)=5;                             /* PCLK, polled Rx/Tx */
  REG(UART+UINTM)=0xf;
  /* PCLK_PSYS 66.7 MHz: 66.7e6/(115200*16) - 1 = 35.19 -> 35 + 3/16 */
  REG(UART+UBRDIV)=35;
  REG(UART+UDIVSLOT)=0x0888;
}

static void dmc_cmd(uint32 cmd) { REG(DMC0+DIRECTCMD)=cmd; }
static void ddr_init(void) {
  for(int n=0;n<9;n++) REG(MP1_DRV(n))=0x0000AAAA;   /* 2x drive strength */
  REG(DMC0+PHYCONTROL0)=0x00101000;           /* DLL parameters */
  REG(DMC0+PHYCONTROL1)=0x00000086;
  REG(DMC0+PHYCONTROL0)=0x00101002;           /* DLL on */
  REG(DMC0+PHYCONTROL0)=0x00101003;           /* DLL start */
  uint32 st, spins=0;
  while(((st=REG(DMC0+PHYSTATUS))&7)!=7)      /* wait for DLL lock */
    if(++spins==0x1000000) die("DMC0 DLL did not lock");
  /* Force the locked value: ctrl_force = ctrl_lock_value. */
  REG(DMC0+PHYCONTROL0)=((st&0x3fc0)<<18)|0x00101003;
  REG(DMC0+CONCONTROL)=0x0FFF2010;            /* auto refresh off */
  REG(DMC0+MEMCONTROL)=0x00202400;            /* DDR2, 32-bit, 1 chip select, BL=4 */
  /* chip_base 0x30, mask 0xF0 (256 MiB), linear, 10 col, 13 row, 8 banks */
  REG(DMC0+MEMCONFIG0)=0x30F01313;
  REG(DMC0+PRECHCONFIG)=0xFF000000;
  REG(DMC0+TIMINGAREF)=0x00000618;            /* 7.8 us refresh */
  REG(DMC0+TIMINGROW)=0x28233287;
  REG(DMC0+TIMINGDATA)=0x23240304;            /* CL=4 */
  REG(DMC0+TIMINGPOWER)=0x09C80232;
  /* JEDEC DDR2 initialisation sequence on chip select 0. */
  dmc_cmd(0x07000000);                        /* NOP (CKE high) */
  dmc_cmd(0x01000000);                        /* precharge all */
  dmc_cmd(0x00020000);                        /* EMRS2 */
  dmc_cmd(0x00030000);                        /* EMRS3 */
  dmc_cmd(0x00010400);                        /* EMRS1: DLL on, DQS# off */
  dmc_cmd(0x00000542);                        /* MRS: DLL reset, CL=4, BL=4 */
  dmc_cmd(0x01000000);                        /* precharge all */
  dmc_cmd(0x05000000);                        /* auto refresh */
  dmc_cmd(0x05000000);                        /* auto refresh */
  dmc_cmd(0x00000442);                        /* MRS: DLL reset release */
  dmc_cmd(0x00010780);                        /* EMRS1: OCD default */
  dmc_cmd(0x00010400);                        /* EMRS1: OCD exit */
  REG(DMC0+CONCONTROL)=0x0FF02030;            /* auto refresh on */
  REG(DMC0+PWRDNCONFIG)=0xFFFF00FF;
  REG(DMC0+MEMCONTROL)=0x00202400;
}

/* Walk a few address lines and both ends of the bank. */
static void ddr_test(void) {
  static const uint32 off[]={0,4,0x100,0x1000,0x10000,0x100000,0x1000000,0x8000000,DRAM_SIZE-4};
  const int n=sizeof(off)/sizeof(off[0]);
  for(int i=0;i<n;i++) REG(DRAM_BASE+off[i])=0x5A5A0000U^(uint32)i^off[i];
  for(int i=0;i<n;i++) {
    uint32 want=0x5A5A0000U^(uint32)i^off[i], got=REG(DRAM_BASE+off[i]);
    if(got!=want) {
      uputs("BL1: DDR mismatch at "); puthex(DRAM_BASE+off[i]);
      uputs(" wrote "); puthex(want); uputs(" read "); puthex(got); uputc('\n');
      die("DDR test failed");
    }
  }
}

static void load_kernel(struct uimage_info *ui) {
  uint32 base=REG(IROM_SDMMC_BASE), ch=0;
  if(base==0xEB000000U) ch=0;         /* SD0: on-board iNand */
  else if(base==0xEB200000U) ch=2;    /* SD2: the SD card slot */
  else { uputs("BL1: iROM boot channel "); puthex(base); uputc('\n'); die("not booted from SD/MMC"); }
  copy_sdmmc_fn copy=(copy_sdmmc_fn)REG(IROM_COPY_SDMMC);
  uint32 *dst=(uint32*)(uintptr)(KERNEL_RUN-UIMAGE_HDR);
  uputs("BL1: SD channel "); putdec(ch); uputs(", xv6 at sector "); putdec(KERNEL_SECTOR); uputc('\n');
  if(!copy(ch,KERNEL_SECTOR,1,dst,0)) die("SD read of the uImage header failed");
  const char *bad=uimage_check((const uchar*)dst,ui);
  if(bad) die(bad);
  if(ui->load!=KERNEL_RUN || ui->entry!=KERNEL_RUN) {
    uputs("BL1: uImage load "); puthex(ui->load); uputs(" entry "); puthex(ui->entry); uputc('\n');
    die("uImage must load and start at 0x30008000");
  }
  uint32 sectors=(UIMAGE_HDR+ui->size+511)>>9;
  uputs("BL1: \""); uputs(ui->name); uputs("\", "); putdec(ui->size); uputs(" bytes, ");
  putdec(sectors); uputs(" sectors\n");
  /* The iROM routine takes a 16-bit block count. */
  for(uint32 done=0; done<sectors; ) {
    uint32 n=sectors-done; if(n>0x8000) n=0x8000;
    if(!copy(ch,KERNEL_SECTOR+done,(uint16)n,dst+done*128,0)) die("SD read of the kernel failed");
    done+=n;
  }
  if(crc32(0,(const uchar*)(uintptr)KERNEL_RUN,ui->size)!=ui->dcrc) die("kernel data CRC mismatch");
  uputs("BL1: data CRC OK\n");
}

void bl1_main(void) {
  struct uimage_info ui;
  power_hold();
  clock_init();
  uart_init();
  uputs("\n\nxv6 BL1 for X210 (S5PV210): APLL 1000 MHz, MPLL 667 MHz, PCLK_PSYS 66.7 MHz\n");
  ddr_init();
  ddr_test();
  uputs("BL1: DMC0 DDR2 256 MiB at 0x30000000 OK\n");
  load_kernel(&ui);
  uint32 n=atag_build((uint32*)(uintptr)ATAG_ADDR,DRAM_BASE,DRAM_SIZE,CMDLINE);
  uputs("BL1: ATAGs at "); puthex(ATAG_ADDR); uputs(" ("); putdec(n); uputs(" bytes), starting xv6 at ");
  puthex(KERNEL_RUN); uputs("\n\n");
  bl1_boot(KERNEL_RUN,MACH_SMDKV210,ATAG_ADDR);
}
