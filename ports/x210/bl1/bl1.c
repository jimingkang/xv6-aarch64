/* xv6 BL1 for S5PV210 boards (FriendlyARM Tiny210, 九鼎 X210; board.h):
   boots xv6 with no U-Boot in between.

     iROM (BL0) ──► this BL1 (SD sector 1, iRAM 0xD0020010)
                ──► xv6 uImage (SD sector 49) copied to DRAM+0x7FC0,
                    so its payload sits at DRAM+0x8000
                ──► jump with r0=0, r1=BOARD_MACH, r2=ATAGs @ DRAM+0x100

   The bring-up mirrors Samsung's S5PV210 U-Boot lowlevel_init/cpu_init.S
   (the base of both the FriendlyARM and the 九鼎 BSP U-Boot):
     PS_HOLD latch, watchdog off, APLL 1000 MHz / MPLL 667 MHz with
     MSYS 200/100, DSYS 166/83, PSYS 133/66.7 MHz, console UART 115200 8N1,
     DMC0 DDR2 (512 MiB at 0x20000000 on Tiny210, 256 MiB at 0x30000000 on
     X210). DMC1 is left uninitialised; ATAG_MEM reports DMC0 only.
   The SD copy uses the iROM's CopySDMMCtoMem (pointer at 0xD0037F98),
   reading from the channel the iROM booted from (0xD0037488). */
#include "kernel/types.h"
#include "bl1lib.h"
#include "board.h"

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
#define MP1_DRV(n) (GPIO+0x3CC+0x20*(n))   /* DMC0 pad drive strength */
/* Console UART (board.h) */
#define UART BOARD_UART
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
#define DRAM_BASE ((uint32)BOARD_DRAM_BASE)
#define KERNEL_SECTOR 49U
#define KERNEL_RUN (DRAM_BASE+0x8000)
#define ATAG_ADDR (DRAM_BASE+0x100)
#define STR2(x) #x
#define STR(x) STR2(x)
#define CMDLINE "console=ttySAC" STR(BOARD_UART_N) ",115200 xv6 bl1"
static uint32 dram_size=BOARD_DRAM_SIZE;

void bl1_boot(uint32 entry, uint32 machid, uint32 atags) __attribute__((noreturn));

/* Stage LEDs. Tiny210: LED1-4 on GPJ2[0:3], active low (FriendlyARM
   mini210/tiny210 BSP). LED n lit = stage n reached:
     1 BL1 running   2 PLLs set   3 DDR OK   4 kernel loaded, jumping
   At start all four blink 3 times; a halt blinks the current stage. */
#define GPJ2CON (GPIO+0x280)
#define GPJ2DAT (GPIO+0x284)
static int stage;
static void delay(uint32 n) { for(volatile uint32 i=0;i<n;i++) ; }
static void leds(uint32 on) {          /* bit k = LED k+1 */
#ifdef BOARD_TINY210
  REG(GPJ2DAT)=(REG(GPJ2DAT)&~0xfU)|(~on&0xfU);
#else
  (void)on;
#endif
}
static void led_stage(int n) { stage=n; leds((1U<<n)-1); }
static void led_init(void) {
#ifdef BOARD_TINY210
  REG(GPJ2CON)=(REG(GPJ2CON)&~0xffffU)|0x1111;  /* GPJ2[0:3] outputs */
#endif
  for(int i=0;i<3;i++) { leds(0xf); delay(0x200000); leds(0); delay(0x200000); }
}

/* Bounded wait: a UART that never drains (no clock) must not hang BL1. */
static int uart_dead;
static void uart_drain(void) {
  for(uint32 n=0; !(REG(UART+UTRSTAT)&4); n++)    /* transmitter empty */
    if(n==0x200000) { uart_dead=1; return; }
}
static void uputc(int c) {
  if(uart_dead) return;
  uart_drain();
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
  for(;;) { leds((1U<<stage)-1); delay(0x400000); leds(0); delay(0x400000); }
}

static uint32 udiv(uint32 a, uint32 b) {   /* no hardware divide on Cortex-A8 */
  uint32 q=0, r=0;
  if(!b) return 0;
  for(int i=31;i>=0;i--) { r=(r<<1)|((a>>i)&1); if(r>=b) { r-=b; q|=1U<<i; } }
  return q;
}
/* PCLK_PSYS from the live clock registers, as the kernel computes it:
   MOUT_PSYS (CLK_SRC0[24]: SCLKMPLL or SCLKA2M) / (HCLK_PSYS_RATIO+1) /
   (PCLK_PSYS_RATIO+1). Works for whatever the iROM left and after our PLLs. */
static uint32 pclk_psys(void) {
  const uint32 fin=24000000;
  uint32 src0=REG(CLK_SRC0), div0=REG(CLK_DIV0), mout;
  if(src0&(1U<<24)) {
    uint32 apll=REG(APLL_CON0), m=(apll>>16)&0x3ff, pd=(apll>>8)&0x3f, sd=apll&7;
    if(!(src0&1)) mout=fin;                     /* APLL_SEL = FINPLL */
    else if(!pd || !sd) return 0;
    else mout=udiv(fin,pd<<(sd-1))*m;
    mout=udiv(mout,((div0>>4)&7)+1);                /* A2M */
  } else if(src0&(1U<<4)) {
    uint32 mpll=REG(MPLL_CON), m=(mpll>>16)&0x3ff, pd=(mpll>>8)&0x3f, sd=mpll&7;
    if(!pd) return 0;
    mout=udiv(fin,pd<<sd)*m;
  } else mout=fin;
  return udiv(udiv(mout,((div0>>24)&15)+1),((div0>>28)&7)+1);
}
static uint32 uart_pclk;
static void uart_baud(void) {
  static const uint16 slot[16]={0x0000,0x0080,0x0808,0x0888,0x2222,0x4924,0x4a52,0x54aa,
                                0x5555,0xd555,0xd5d5,0xddd5,0xdddd,0xdfdd,0xdfdf,0xffdf};
  uart_pclk=pclk_psys();
  if(uart_pclk<1000000) uart_pclk=66700000;
  uint32 t=udiv(uart_pclk+57600,115200);    /* = 16*(UBRDIV+1) + slots, rounded */
  REG(UART+UBRDIV)=(t>>4)-1;
  REG(UART+UDIVSLOT)=slot[t&15];
}

static void power_hold(void) {
  /* X210 has a soft power key: latch PS_HOLD high or the board turns off
     when the key is released. */
  REG(PS_HOLD_CONTROL)|=0x301;
  REG(WTCON)=0;
}

static int pll_locked;
static void clock_init(void) {
  uart_drain();                       /* let the last character finish */
  REG(CLK_SRC0)=0;                    /* everything from XXTI 24 MHz meanwhile */
  REG(APLL_LOCK)=0xFFFF;
  REG(MPLL_LOCK)=0xFFFF;
  REG(CLK_DIV0)=CLK_DIV0_VAL;
  REG(APLL_CON0)=PLL_EN|(125U<<16)|(3U<<8)|1;   /* 125*24/(3*2^0) = 1000 MHz */
  REG(MPLL_CON)=PLL_EN|(667U<<16)|(12U<<8)|1;   /* 667*24/(12*2^1) = 667 MHz */
  /* Samsung's U-Boot just waits; poll LOCKED but never hang on it. */
  for(uint32 n=0; !(REG(APLL_CON0)&PLL_LOCKED) && n<0x100000; n++) ;
  for(uint32 n=0; !(REG(MPLL_CON)&PLL_LOCKED) && n<0x100000; n++) ;
  pll_locked=(REG(APLL_CON0)&REG(MPLL_CON)&PLL_LOCKED)!=0;
  delay(0x10000);
  /* APLL_SEL, MPLL_SEL = FOUT; MSYS from APLL, DSYS/PSYS from MPLL. */
  REG(CLK_SRC0)=0x00000011;
}

static void uart_init(void) {
  REG(BOARD_UART_GPIOCON)=(REG(BOARD_UART_GPIOCON)&~0xffU)|0x22;  /* RXD/TXD */
  REG(UART+UFCON)=0;
  REG(UART+UMCON)=0;
  REG(UART+ULCON)=3;                            /* 8N1 */
  REG(UART+UCON)=5;                             /* PCLK, polled Rx/Tx */
  REG(UART+UINTM)=0xf;
  uart_baud();   /* 66.7 MHz after clock_init: UBRDIV 35, UDIVSLOT 0x0888 */
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
  REG(DMC0+MEMCONFIG0)=BOARD_MEMCONFIG0;      /* base, size, rows/cols/banks */
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

/* If the chips have one row bit fewer than MEMCONFIG0 says, the upper half
   aliases the lower half: fall back to half the size (xv6 uses 128 MiB). */
static void ddr_size(void) {
  volatile uint32 *lo=(uint32*)(uintptr)DRAM_BASE, *hi=(uint32*)(uintptr)(DRAM_BASE+dram_size/2);
  *lo=0x11111111; *hi=0x22222222;
  if(*lo!=0x11111111) {
    uputs("BL1: warning: DDR above "); puthex(DRAM_BASE+dram_size/2);
    uputs(" aliases the lower half, using the lower half only\n");
    dram_size/=2;
  }
}
/* Walk a few address lines and both ends of the bank. */
static void ddr_test(void) {
  uint32 off[]={0,4,0x100,0x1000,0x10000,0x100000,0x1000000,0x8000000,dram_size-4};
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
    uputs("BL1: expected "); puthex(KERNEL_RUN); uputc('\n');
    die("uImage load/entry address does not match this board");
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
  led_init();
  led_stage(1);
  /* Console first, at whatever clocks the iROM left, so that a wrong PLL
     setup can be told apart from a wrong terminal/baud setting. */
  uart_init();
  uputs("\n\nxv6 BL1 for " BOARD_NAME "\n");
  uputs("BL1: iROM clocks: CLK_SRC0 "); puthex(REG(CLK_SRC0)); uputs(" CLK_DIV0 "); puthex(REG(CLK_DIV0));
  uputs(" PCLK_PSYS "); putdec(uart_pclk); uputs(" Hz\n");
  int early_dead=uart_dead;
  clock_init();
  uart_baud();
  uart_dead=0;                        /* retry the console on the new clocks */
  if(early_dead) uputs("\nBL1: console did not drain on the iROM clocks\n");
  led_stage(2);
  uputs("BL1: PLLs set: APLL_CON0 "); puthex(REG(APLL_CON0)); uputs(" MPLL_CON "); puthex(REG(MPLL_CON));
  uputs(" PCLK_PSYS "); putdec(uart_pclk); uputs(" Hz");
  uputs(pll_locked ? "\n" : " (LOCKED bit never set)\n");
  ddr_init();
  ddr_size();
  ddr_test();
  led_stage(3);
  uputs("BL1: DMC0 DDR2 "); putdec(dram_size>>20); uputs(" MiB at "); puthex(DRAM_BASE); uputs(" OK\n");
  load_kernel(&ui);
  led_stage(4);
  uint32 n=atag_build((uint32*)(uintptr)ATAG_ADDR,DRAM_BASE,dram_size,CMDLINE);
  uputs("BL1: ATAGs at "); puthex(ATAG_ADDR); uputs(" ("); putdec(n); uputs(" bytes), starting xv6 at ");
  puthex(KERNEL_RUN); uputs("\n\n");
  bl1_boot(KERNEL_RUN,BOARD_MACH,ATAG_ADDR);
}
