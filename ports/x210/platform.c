/* S5PV210 (Cortex-A8) platform glue: FriendlyARM Tiny210 / 九鼎 X210.
   Inherits from BL1/U-Boot: clocks (APLL 1 GHz, MPLL 667 MHz), DDR2,
   console UART pinmux and 115200 8N1 (board.h).
   Initialises here: watchdog (off), VIC0-3, PWM timer 4 (10 ms tick).
   X210_QEMU builds the same kernel for QEMU's Cortex-A8 "cubieboard"
   (Allwinner A10 peripherals), the only Cortex-A8 machine QEMU has. */
#include "kernel/types.h"
#include "kernel/param.h"
#include "kernel/memlayout.h"
#include "kernel/aarch64.h"
#include "kernel/spinlock.h"
#include "kernel/proc.h"
#include "kernel/defs.h"
#include "kernel/sleeplock.h"
#include "kernel/fs.h"
#include "kernel/buf.h"
#include "kernel/file.h"
#include "kernel/device.h"
#include "atag.h"
#ifndef X210_QEMU
#include "lcd.h"
#endif
#include "kernel/arch/arm/fdt.h"
_Static_assert(sizeof(struct trapframe)==272, "ARM exception frame size");
_Static_assert(__builtin_offsetof(struct trapframe,x30)==240, "ARM user LR slot");
_Static_assert(__builtin_offsetof(struct trapframe,elr)==248, "ARM PC slot");
_Static_assert(__builtin_offsetof(struct context,x30)==104, "ARM context LR slot");
_Static_assert(DRAM_BASE>=MAXVA, "DRAM must lie above the user window");
#define REG(a) (*(volatile uint32*)(uintptr)(a))
#define HZ_TICK 100      /* timer interrupts per second */
#ifdef X210_QEMU
#define UART 0x01C28000  /* A10 UART0, 16550 with 4-byte stride */
#define PIC 0x01C20400
#define PIT 0x01C20C00
#define TIMER_IRQ 22
#define BOARD "QEMU cubieboard (Cortex-A8 stand-in for S5PV210)"
#else
#define UART UART0_PA    /* board.h console UART */
#define CLKCON 0xE0100000
#define PWM 0xE2500000
#define WDT 0xE2700000
#define VIC(n) (0xF2000000U + 0x100000U*(n))
#define TIMER_IRQ TIMER0_IRQ  /* VIC0 bit 25: PWM timer 4 */
#define BOARD BOARD_NAME
/* Samsung UART registers */
#define UFCON 0x08
#define UTRSTAT 0x10
#define UFSTAT 0x18
#define UTXH 0x20
#define URXH 0x24
#define UINTP 0x30
#define UINTSP 0x34
#define UINTM 0x38
/* PL192 VIC registers */
#define VIC_IRQSTATUS 0x000
#define VIC_INTSELECT 0x00c
#define VIC_INTENABLE 0x010
#define VIC_INTENCLEAR 0x014
#define VIC_SOFTINTCLEAR 0x01c
#define VIC_ADDRESS 0xf00
/* PWM timer registers (timer 4 has no output pin) */
#define TCFG0 0x00
#define TCFG1 0x04
#define TCON 0x08
#define TCNTB4 0x3c
#define TCNTO4 0x40
#define TINT_CSTAT 0x44
#endif
/* Caches stay off in this bring-up, and kinit junk-fills every free page
   uncached, so only the first USE_DRAM bytes of DDR are handed to the
   allocator (X210 has 512 MiB in two banks; ATAG_MEM is still reported). */
#define USE_DRAM (128U<<20)
#ifndef X210_QEMU
_Static_assert(LCD_FB_OFFSET>=USE_DRAM, "LCD frame buffer overlaps the page allocator");
#endif

extern uchar ramdisk_start[], ramdisk_end[];
extern char end[];
struct spinlock tickslock;
uint ticks;
uint x210_phystop = DRAM_BASE + (128U<<20);
static uint timer_hz, timer_reload;
static volatile uint64 timer_wraps;
static struct bootparams bootp;
static struct fdt_info bootfdt;

#define SECTION_DEV(pa) ((pa) | (1<<10) | (1<<4) | 2)  /* priv RW, XN, strongly ordered */
void x210_map_devices(pagetable_t p) {
#ifdef X210_QEMU
  p[0x01C00000>>20]=SECTION_DEV(0x01C00000);
#else
  p[CLKCON>>20]=SECTION_DEV(CLKCON);                  /* clock controller */
  p[PWM>>20]=SECTION_DEV(PWM);                        /* PWM timers */
  p[WDT>>20]=SECTION_DEV(WDT);                        /* watchdog */
  p[UART>>20]=SECTION_DEV(UART & ~0xFFFFFU);          /* UART0-3 */
  for(int n=0;n<4;n++) p[VIC(n)>>20]=SECTION_DEV(VIC(n));
  p[0xE0200000>>20]=SECTION_DEV(0xE0200000);          /* GPIO (LCD pins) */
  p[0xF8000000>>20]=SECTION_DEV(0xF8000000);          /* FIMD */
  /* LCD frame buffer: privileged, XN, Normal non-cacheable (TEX=001). */
  for(uint a=DRAM_BASE+LCD_FB_OFFSET; a<DRAM_BASE+LCD_FB_OFFSET+LCD_FB_SIZE; a+=0x100000)
    p[a>>20]=a | (1<<12) | (1<<10) | (1<<4) | 2;
#endif
}

#ifdef X210_QEMU
void uartinit(void) { REG(UART+12) &= ~128U; REG(UART+4) = 0; }
int uartgetc(void) { return (REG(UART+20)&1) ? REG(UART)&255 : -1; }
void uartputc_sync(int c) { while(!(REG(UART+20)&32)) {} REG(UART)=(uchar)c; }
#else
/* Keep U-Boot's ULCON/UCON/UBRDIV (115200 8N1 from PCLK_PSYS); mask the
   UART's own interrupt sources. Input is drained on each timer tick. */
void uartinit(void) {
  REG(UART+UINTM)=0xf;
  REG(UART+UINTSP)=0xf;
  REG(UART+UINTP)=0xf;
}
static int uart_fifo(void) { return REG(UART+UFCON)&1; }
int uartgetc(void) {
  int ready = uart_fifo() ? (REG(UART+UFSTAT)&0x1ff)!=0 : (REG(UART+UTRSTAT)&1);
  return ready ? (int)(REG(UART+URXH)&255) : -1;
}
void uartputc_sync(int c) {
  if(uart_fifo()) { while(REG(UART+UFSTAT)&(1U<<24)) {} }  /* Tx FIFO full */
  else { while(!(REG(UART+UTRSTAT)&2)) {} }                /* Tx buffer empty */
  REG(UART+UTXH)=(uchar)c;
  lcd_putc(c);                                             /* mirror on the LCD */
}
#endif
void uartputc(int c) { uartputc_sync(c); }
void uartintr(void) { int c; while((c=uartgetc())>=0) consoleintr(c); }
void boot_uart_mark(int c) { (void)c; }
void fat32rw(struct buf *b,int write) {
  uint64 off=(uint64)b->blockno*BSIZE;
  if(b->dev!=ROOTDEV || off+BSIZE>(uintptr)(ramdisk_end-ramdisk_start)) panic("ramdisk bounds");
  if(write) memmove(ramdisk_start+(uintptr)off,b->data,BSIZE);
  else memmove(b->data,ramdisk_start+(uintptr)off,BSIZE);
}

#ifndef X210_QEMU
static void wdt_disable(void) { REG(WDT)=0; }  /* WTCON: timer and reset off */
/* PCLK_PSYS feeds the PWM block: MOUT_PSYS / (HCLK_PSYS_RATIO+1) /
   (PCLK_PSYS_RATIO+1), MOUT_PSYS = SCLKMPLL or SCLKA2M (CLK_SRC0[24]).
   The X210 U-Boot sets MPLL 667 MHz -> 133 MHz -> 66.7 MHz. */
static uint pclk_psys(void) {
  const uint fin=24000000;
  uint src0=REG(CLKCON+0x200), div0=REG(CLKCON+0x300), mout;
  if(src0&(1U<<24)) {
    uint apll=REG(CLKCON+0x100), m=(apll>>16)&0x3ff, pd=(apll>>8)&0x3f, s=apll&7;
    if(!pd || !s) return 0;
    mout=(uint)((uint64)fin*m/(pd<<(s-1)))/(((div0>>4)&7)+1);   /* A2M */
  } else if(src0&(1U<<4)) {
    uint mpll=REG(CLKCON+0x108), m=(mpll>>16)&0x3ff, pd=(mpll>>8)&0x3f, s=mpll&7;
    if(!pd) return 0;
    mout=(uint)((uint64)fin*m/(pd<<s));
  } else mout=fin;
  return mout/(((div0>>24)&15)+1)/(((div0>>28)&7)+1);
}
static uint timer_clock(void) {
  uint pclk=pclk_psys();
  return (pclk<10000000 || pclk>200000000) ? 66700000 : pclk;
}
#endif

static void interrupts_init(void) {
#ifdef X210_QEMU
  for(int i=0;i<3;i++) { REG(PIC+0x40+4*i)=0; REG(PIC+0x50+4*i)=~0U; REG(PIC+0x30+4*i)=0; }
  REG(PIC+0x40)=1U<<TIMER_IRQ;   /* enable  */
  REG(PIC+0x50)=~(1U<<TIMER_IRQ); /* unmask */
#else
  for(int n=0;n<4;n++) {
    REG(VIC(n)+VIC_INTENCLEAR)=~0U;   /* disable everything U-Boot left on */
    REG(VIC(n)+VIC_SOFTINTCLEAR)=~0U;
    REG(VIC(n)+VIC_INTSELECT)=0;      /* all IRQ, no FIQ */
    REG(VIC(n)+VIC_ADDRESS)=0;        /* reset the priority logic */
  }
  REG(VIC(0)+VIC_INTENABLE)=1U<<TIMER_IRQ;
#endif
  dsb();
}

static void timer_start(void) {
#ifdef X210_QEMU
  timer_hz=24000000; timer_reload=timer_hz/HZ_TICK;
  REG(PIT+0x10)=0; REG(PIT)=0; REG(PIT+4)=0x3f;
  REG(PIT+0x14)=timer_reload;
  REG(PIT+0x10)=(1<<2)|(1<<1)|1;   /* OSC24M, reload, enable */
  REG(PIT)=1;
#else
  uint pclk=timer_clock();
  /* Prescaler 1 (timers 2-4) = 0 and divider 1/1: the timer runs at
     PCLK_PSYS. Counters are 32-bit, so a 10 ms reload fits. */
  timer_hz=pclk; timer_reload=timer_hz/HZ_TICK;
  REG(PWM+TCON)&=~(7U<<20);                         /* stop timer 4 */
  REG(PWM+TCFG0)&=~(0xffU<<8);
  REG(PWM+TCFG1)&=~(0xfU<<16);
  REG(PWM+TCNTB4)=timer_reload-1;
  REG(PWM+TINT_CSTAT)=(REG(PWM+TINT_CSTAT)&0x1f)|(1U<<9);  /* clear pending */
  REG(PWM+TINT_CSTAT)=(REG(PWM+TINT_CSTAT)&0x1f)|(1U<<4);  /* enable timer 4 IRQ */
  REG(PWM+TCON)=(REG(PWM+TCON)&~(7U<<20))|(1U<<21);         /* manual update */
  REG(PWM+TCON)=(REG(PWM+TCON)&~(7U<<20))|(1U<<22)|(1U<<20); /* auto-reload, start */
#endif
}
static int timer_ack(void) {
#ifdef X210_QEMU
  if(!(REG(PIT+4)&1)) return 0;
  REG(PIT+4)=1;
#else
  uint cstat=REG(PWM+TINT_CSTAT);
  if(!(cstat&(1U<<9))) return 0;
  REG(PWM+TINT_CSTAT)=(cstat&0x1f)|(1U<<9);       /* W1C, keep enables */
#endif
  timer_wraps++;
  return 1;
}
/* Monotonic counter for r_cntvct_el0(): wraps * reload + elapsed. */
uint arm32_counter_hz(void) { return timer_hz ? timer_hz : 24000000; }
uint64 arm32_counter(void) {
  uint64 w; uint elapsed;
  do {
    w=timer_wraps;
#ifdef X210_QEMU
    elapsed=timer_reload-REG(PIT+0x18);
#else
    elapsed=(timer_reload-1)-REG(PWM+TCNTO4);
#endif
  } while(w!=timer_wraps);
  return w*timer_reload+elapsed;
}

/* Only probe addresses in the first DRAM bank: a stray r2 pointing at
   unpopulated space would fault with the MMU off. */
static int in_dram(uint a) { return a>=DRAM_BASE && a<DRAM_BASE+(128U<<20)-4096 && !(a&3); }
static void read_bootparams(uint r2) {
  bootp.kind=0;
  if(in_dram(r2) && *(volatile uint32*)(uintptr)r2==0xedfe0dd0U) {   /* FDT magic, BE */
    if(fdt_parse(r2,&bootfdt)==0) {
      bootp.kind=2; bootp.where=r2;
      bootp.nmem=bootfdt.mem_size ? 1 : 0;
      bootp.mem_base[0]=(uint32)bootfdt.mem_base; bootp.mem_size[0]=(uint32)bootfdt.mem_size;
      safestrcpy(bootp.cmdline,bootfdt.bootargs,sizeof(bootp.cmdline));
      safestrcpy(bootp.model,bootfdt.model,sizeof(bootp.model));
    }
    return;
  }
  if(in_dram(r2) && atag_parse(r2,&bootp)==0) return;
  /* r2 lost (e.g. `go`): try the conventional ATAG address. */
  if(r2!=DRAM_BASE+0x100 && atag_parse(DRAM_BASE+0x100,&bootp)==0) return;
  bootp.kind=0;
}

static void boot_info(uint r0, uint machid, uint r2) {
#ifdef X210_QEMU
  printf("boot: r0=%x machid=%d r2=%x\n",r0,machid,r2);
#else
  printf("boot: r0=%x machid=%d%s r2=%x\n",r0,machid,
         machid==BOARD_MACH ? " (" BOARD_MACH_NAME ")" : "",r2);
#endif
  if(bootp.kind==0) {
    printf("boot: no ATAG list or DTB at r2 (plain `go`?), using defaults\n");
    return;
  }
  if(bootp.kind==1) printf("atags: at %x%s\n",bootp.where,bootp.where!=r2 ? " (r2 ignored)" : "");
  else printf("dtb: at %x, model \"%s\"\n",bootp.where,bootp.model);
  for(int i=0;i<bootp.nmem;i++)
    printf("%s: memory %x + %d MiB\n",bootp.kind==1 ? "atags" : "dtb",
           bootp.mem_base[i],bootp.mem_size[i]>>20);
  if(bootp.cmdline[0]) printf("%s: cmdline \"%s\"\n",bootp.kind==1 ? "atags" : "dtb",bootp.cmdline);
  if(bootp.initrd_size) printf("atags: initrd %x + %d bytes (ignored)\n",bootp.initrd_base,bootp.initrd_size);
}

void x210_main(uint r0, uint machid, uint r2) {
#ifndef X210_QEMU
  wdt_disable();
#endif
  uartinit();
  uartputc_sync('\r'); uartputc_sync('\n');
  /* Read the boot parameters before kinit hands their pages out
     (they sit below the kernel and are never freed, but bounds come first). */
  read_bootparams(r2);
  x210_phystop=DRAM_BASE+USE_DRAM;
  for(int i=0;i<bootp.nmem;i++) {
    uint64 base=bootp.mem_base[i], top=base+bootp.mem_size[i];
    if(base<=DRAM_BASE && top>DRAM_BASE && top<x210_phystop) x210_phystop=(uint)top;
  }
  if((uintptr)end>=x210_phystop) x210_phystop=PGROUNDUP((uintptr)end)+(16U<<20);
  /* Turn the MMU on before the allocator runs, so LDREX/STREX in the
     spinlocks see Normal memory rather than MMU-off Strongly-ordered. */
  kvminit(); kvminithart(); kinit1(end,P2V(PHYSTOP));
  device_init(); consoleinit(); ttyinit(); printfinit();
  printf("\nxv6 ARM32 / %s\n",BOARD);
  boot_info(r0,machid,r2);
#ifndef X210_QEMU
  printf("timer: PWM4 from PCLK_PSYS %d Hz\n",timer_clock());
#endif
  printf("kernel %x-%x, DRAM %x-%x\n",KERNPA,(uint)(uintptr)end,DRAM_BASE,x210_phystop);
#ifndef X210_QEMU
  lcd_init();
#endif
  printf("single CPU, ARMv7 MMU, embedded RAM disk\n");
  procinit(); initlock(&tickslock,"ticks");
  binit(); iinit(); fileinit(); userinit();
  interrupts_init(); timer_start();
  scheduler();
}

/* Exception entry points called from ports/t113/vectors.S. */
void t113_svc(struct trapframe *tf) {
  struct proc *p=myproc();
  if(!p || (tf->spsr&31)!=16) panic("svc outside user");
  if(tf!=p->trapframe) panic("svc frame");
  if(p->killed) exit(-1);
  intr_on(); syscall();
  if(p->killed) exit(-1);
  if(resched_pending()) yield();
  intr_off();
}
void t113_irq(struct trapframe *tf) {
#ifdef X210_QEMU
  int timer=1;
#else
  int timer=(REG(VIC(0)+VIC_IRQSTATUS)>>TIMER_IRQ)&1;
#endif
  if(timer && timer_ack()) {
    static uint sub;
    if(++sub==10) { sub=0; acquire(&tickslock); ticks++; wakeup(&ticks); release(&tickslock); }
    uartintr(); sched_tick();
  }
#ifndef X210_QEMU
  for(int n=0;n<4;n++) REG(VIC(n)+VIC_ADDRESS)=0;
#endif
  dsb();
  struct proc *p=myproc();
  if(p && p->state==RUNNING && resched_pending()) yield();
  if(p && (tf->spsr&31)==16 && p->killed) exit(-1);
}
void t113_fault(uint kind,uint lr,uint psr) {
  uint far, status;
  asm volatile("mrc p15, 0, %0, c6, c0, 0":"=r"(far));
  asm volatile("mrc p15, 0, %0, c5, c0, 0":"=r"(status));
  printf("ARM fault kind=%d lr=%x cpsr=%x far=%x dfsr=%x\n",kind,lr,psr,far,status);
  if((psr&31)==16 && myproc()) exit(-1);
  panic("ARM fault");
}
void send_resched_ipi(int cpu) { (void)cpu; }
void net_udp_closeproc(int pid) { (void)pid; }
