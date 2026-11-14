/* BeagleBone Black (TI AM335x, Cortex-A8) platform glue.
   Inherits from U-Boot: DDR3, UART0 pinmux/clock/115200 8N1, L4 clocks.
   Initialises here: WDT1 (disabled), INTC, DMTimer2 (10 ms tick).
   AM335X_QEMU builds the same kernel for QEMU's Cortex-A8 "cubieboard"
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
#include "kernel/arch/arm/fdt.h"
_Static_assert(sizeof(struct trapframe)==272, "ARM exception frame size");
_Static_assert(__builtin_offsetof(struct trapframe,x30)==240, "ARM user LR slot");
_Static_assert(__builtin_offsetof(struct trapframe,elr)==248, "ARM PC slot");
_Static_assert(__builtin_offsetof(struct context,x30)==104, "ARM context LR slot");
#define REG(a) (*(volatile uint32*)(uintptr)(a))
#define HZ_TICK 100      /* timer interrupts per second */
#ifdef AM335X_QEMU
#define UART 0x01C28000  /* A10 UART0, 16550 with 4-byte stride */
#define PIC 0x01C20400
#define PIT 0x01C20C00
#define TIMER_IRQ 22
#define BOARD "QEMU cubieboard (Cortex-A8 stand-in for AM335x)"
#else
#define UART UART0_PA    /* 0x44E09000 */
#define CM_PER 0x44E00000
#define CM_DPLL 0x44E00500
#define CTRL 0x44E10000
#define WDT1 0x44E35000
#define DMTIMER2 0x48040000
#define INTC 0x48200000
#define TIMER_IRQ TIMER0_IRQ  /* TINT2 = 68 */
#define BOARD "BeagleBone Black (TI AM335x)"
#endif
/* Caches stay off in this bring-up, and kinit junk-fills every free page
   uncached, so only the first USE_DRAM bytes of DDR are handed to the
   allocator (BBB has 512 MiB; the DTB /memory size is still reported). */
#define USE_DRAM (128U<<20)

extern uchar ramdisk_start[], ramdisk_end[];
extern char end[];
struct spinlock tickslock;
uint ticks;
uint am335x_phystop = DRAM_BASE + (128U<<20);
static uint timer_hz, timer_reload;
static volatile uint64 timer_wraps;
static struct fdt_info bootfdt;

void am335x_map_devices(pagetable_t p) {
  /* 1 MiB privileged, execute-never, strongly-ordered sections. */
#ifdef AM335X_QEMU
  p[0x01C00000>>20]=0x01C00000 | (1<<10) | (1<<4) | 2;
#else
  p[0x44E00000>>20]=0x44E00000 | (1<<10) | (1<<4) | 2; /* CM, CTRL, UART0, WDT1 */
  p[0x48000000>>20]=0x48000000 | (1<<10) | (1<<4) | 2; /* L4_PER: DMTimer2 */
  p[0x48200000>>20]=0x48200000 | (1<<10) | (1<<4) | 2; /* INTC */
#endif
}

void uartinit(void) {
  /* Keep U-Boot's divisor and mode; clear DLAB and mask UART interrupts.
     Input is drained on each timer tick. */
  REG(UART+12) &= ~128U;
  REG(UART+4) = 0;
}
int uartgetc(void) { return (REG(UART+20)&1) ? REG(UART)&255 : -1; }
void uartputc_sync(int c) { while(!(REG(UART+20)&32)) {} REG(UART)=(uchar)c; }
void uartputc(int c) { uartputc_sync(c); }
void uartintr(void) { int c; while((c=uartgetc())>=0) consoleintr(c); }
void boot_uart_mark(int c) { (void)c; }
void fat32rw(struct buf *b,int write) {
  uint64 off=(uint64)b->blockno*BSIZE;
  if(b->dev!=ROOTDEV || off+BSIZE>(uintptr)(ramdisk_end-ramdisk_start)) panic("ramdisk bounds");
  if(write) memmove(ramdisk_start+(uintptr)off,b->data,BSIZE);
  else memmove(b->data,ramdisk_start+(uintptr)off,BSIZE);
}

#ifndef AM335X_QEMU
static void wdt_disable(void) {
  /* U-Boot with CONFIG_WATCHDOG_AUTOSTART leaves WDT1 running (~60 s).
     The documented stop sequence writes 0xAAAA then 0x5555 to WSPR. */
  REG(WDT1+0x48)=0xAAAA; while(REG(WDT1+0x34)&(1<<4)) ;
  REG(WDT1+0x48)=0x5555; while(REG(WDT1+0x34)&(1<<4)) ;
}
static uint osc_hz(void) {
  /* CONTROL_STATUS SYSBOOT[15:14]: crystal frequency latched at reset. */
  static const uint f[4]={19200000,24000000,25000000,26000000};
  return f[(REG(CTRL+0x40)>>22)&3];
}
static void dmtimer_wait(void) { while(REG(DMTIMER2+0x48)) ; } /* TWPS */
#endif

static void interrupts_init(void) {
#ifdef AM335X_QEMU
  for(int i=0;i<3;i++) { REG(PIC+0x40+4*i)=0; REG(PIC+0x50+4*i)=~0U; REG(PIC+0x30+4*i)=0; }
  REG(PIC+0x40)=1U<<TIMER_IRQ;   /* enable  */
  REG(PIC+0x50)=~(1U<<TIMER_IRQ); /* unmask */
#else
  REG(INTC+0x10)=2;                   /* INTC_SYSCONFIG.SoftReset */
  while(!(REG(INTC+0x14)&1)) ;        /* INTC_SYSSTATUS.ResetDone */
  REG(INTC+0x68)=0xff;                /* priority threshold disabled */
  for(int n=0;n<4;n++) REG(INTC+0x8c+0x20*n)=~0U;   /* MIR_SET: mask all */
  REG(INTC+0x100+4*TIMER_IRQ)=0;      /* ILR: priority 0, route to IRQ */
  REG(INTC+0x88+0x20*(TIMER_IRQ/32))=1U<<(TIMER_IRQ%32); /* MIR_CLEAR */
  REG(INTC+0x48)=1;                   /* NEWIRQAGR */
#endif
  dsb();
}

static void timer_start(void) {
#ifdef AM335X_QEMU
  timer_hz=24000000; timer_reload=timer_hz/HZ_TICK;
  REG(PIT+0x10)=0; REG(PIT)=0; REG(PIT+4)=0x3f;
  REG(PIT+0x14)=timer_reload;
  REG(PIT+0x10)=(1<<2)|(1<<1)|1;   /* OSC24M, reload, enable */
  REG(PIT)=1;
#else
  timer_hz=osc_hz(); timer_reload=timer_hz/HZ_TICK;
  REG(CM_DPLL+0x08)=1;                /* CLKSEL_TIMER2_CLK = CLK_M_OSC */
  REG(CM_PER+0x80)=2;                 /* CM_PER_TIMER2_CLKCTRL: enable */
  while((REG(CM_PER+0x80)>>16)&3) ;   /* IDLEST: fully functional */
  REG(DMTIMER2+0x10)=1;               /* TIOCP_CFG.SoftReset */
  while(REG(DMTIMER2+0x10)&1) ;
  REG(DMTIMER2+0x54)=0;               /* TSICR: non-posted writes */
  REG(DMTIMER2+0x40)=0U-timer_reload; dmtimer_wait();   /* TLDR */
  REG(DMTIMER2+0x3c)=0U-timer_reload; dmtimer_wait();   /* TCRR */
  REG(DMTIMER2+0x28)=7;               /* IRQSTATUS: clear */
  REG(DMTIMER2+0x2c)=2;               /* IRQENABLE_SET: overflow */
  REG(DMTIMER2+0x38)=3; dmtimer_wait();  /* TCLR: auto-reload, start */
#endif
}
static int timer_ack(void) {
#ifdef AM335X_QEMU
  if(!(REG(PIT+4)&1)) return 0;
  REG(PIT+4)=1;
#else
  if(!(REG(DMTIMER2+0x28)&2)) return 0;
  REG(DMTIMER2+0x28)=2;
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
#ifdef AM335X_QEMU
    elapsed=timer_reload-REG(PIT+0x18);
#else
    elapsed=REG(DMTIMER2+0x3c)-(0U-timer_reload);
#endif
  } while(w!=timer_wraps);
  return w*timer_reload+elapsed;
}

static void boot_info(uint r0, uint machid, uint dtb) {
  printf("boot: r0=%x machid=%x dtb=%x\n",r0,machid,dtb);
  if(!bootfdt.valid) {
    printf("boot: no valid DTB at r2 (plain `go`?), using defaults\n");
    return;
  }
  printf("dtb: %d bytes, model \"%s\"\n",bootfdt.size,bootfdt.model);
  printf("dtb: compatible \"%s\"\n",bootfdt.compatible);
  printf("dtb: memory %x + %d MiB\n",(uint)bootfdt.mem_base,(uint)(bootfdt.mem_size>>20));
  if(bootfdt.bootargs[0]) printf("dtb: bootargs \"%s\"\n",bootfdt.bootargs);
}

void am335x_main(uint r0, uint machid, uint dtb) {
#ifndef AM335X_QEMU
  wdt_disable();
#endif
  uartinit();
  uartputc_sync('\r'); uartputc_sync('\n');
  /* Read the DTB before kinit hands its pages to the allocator. */
  am335x_phystop=DRAM_BASE+USE_DRAM;
  if(fdt_parse(dtb,&bootfdt)==0 && bootfdt.mem_size) {
    uint64 top=bootfdt.mem_base+bootfdt.mem_size;
    if(bootfdt.mem_base<=DRAM_BASE && top>DRAM_BASE && top<am335x_phystop)
      am335x_phystop=(uint)top;
  }
  if((uintptr)end>=am335x_phystop) am335x_phystop=PGROUNDUP((uintptr)end)+(16U<<20);
  /* Turn the MMU on before the allocator runs, so LDREX/STREX in the
     spinlocks see Normal memory rather than MMU-off Strongly-ordered. */
  kvminit(); kvminithart(); kinit1(end,P2V(PHYSTOP));
  device_init(); consoleinit(); ttyinit(); printfinit();
  printf("\nxv6 ARM32 / %s\n",BOARD);
  boot_info(r0,machid,dtb);
  printf("kernel %x-%x, DRAM %x-%x\n",KERNPA,(uint)(uintptr)end,DRAM_BASE,am335x_phystop);
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
#ifndef AM335X_QEMU
  uint sir=REG(INTC+0x40);            /* INTC_SIR_IRQ */
  uint irq=(sir&0xffffff80U) ? ~0U : (sir&0x7f);
#else
  uint irq=TIMER_IRQ;
#endif
  if(irq==TIMER_IRQ && timer_ack()) {
    static uint sub;
    if(++sub==10) { sub=0; acquire(&tickslock); ticks++; wakeup(&ticks); release(&tickslock); }
    uartintr(); sched_tick();
  }
#ifndef AM335X_QEMU
  REG(INTC+0x48)=1;                   /* NEWIRQAGR */
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
