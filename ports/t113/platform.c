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
_Static_assert(sizeof(struct trapframe)==272, "ARM exception frame size");
_Static_assert(__builtin_offsetof(struct trapframe,x30)==240, "ARM user LR slot");
_Static_assert(__builtin_offsetof(struct trapframe,elr)==248, "ARM PC slot");
_Static_assert(__builtin_offsetof(struct context,x30)==104, "ARM context LR slot");
#define REG(a) (*(volatile uint32*)(uintptr)(a))
#ifdef T113_QEMU
#define UART 0x09000000
#define GICD 0x08000000
#define GICC 0x08010000
#define TIMER_IRQ 30
#else
#define UART UART0_PA
#define GICD 0x03021000
#define GICC 0x03022000
#define TIMER_IRQ TIMER0_IRQ
#define TIMER 0x02050000
#endif
extern uchar ramdisk_start[], ramdisk_end[];
extern char end[];
struct spinlock tickslock;
uint ticks;
void uartinit(void) {
#ifndef T113_QEMU
  /* Preserve U-Boot's pinmux, clock and divisor; disable UART interrupts.
     Input is drained every timer tick, avoiding BSP IRQ routing differences. */
  REG(UART+12) &= ~128U;
  REG(UART+4) = 0;
#endif
}
int uartgetc(void) {
#ifdef T113_QEMU
  return (REG(UART+0x18)&16) ? -1 : REG(UART)&255;
#else
  return (REG(UART+20)&1) ? REG(UART)&255 : -1;
#endif
}
void uartputc_sync(int c) {
#ifdef T113_QEMU
  while(REG(UART+0x18)&32) ;
#else
  while(!(REG(UART+20)&32)) ;
#endif
  REG(UART)=(uchar)c;
}
void uartputc(int c) { uartputc_sync(c); }
void uartintr(void) { int c; while((c=uartgetc())>=0) consoleintr(c); }
void boot_uart_mark(int c) { (void)c; }
void fat32rw(struct buf *b,int write) {
  uint64 off=(uint64)b->blockno*BSIZE;
  if(b->dev!=ROOTDEV || off+BSIZE>(uintptr)(ramdisk_end-ramdisk_start)) panic("ramdisk bounds");
  if(write) memmove(ramdisk_start+(uintptr)off,b->data,BSIZE);
  else memmove(b->data,ramdisk_start+(uintptr)off,BSIZE);
}
static void interrupts_init(void) {
  REG(GICD)=0; REG(GICC)=0;
  uint lines=((REG(GICD+4)&31)+1)*32;
  for(uint i=0;i<lines;i+=32) { REG(GICD+0x180+i/8)=~0U; REG(GICD+0x280+i/8)=~0U; }
  /* Put the timer in Group 0 and route to CPU0. Both secure and non-secure
     U-Boot builds can enable their accessible interrupt group. */
  REG(GICD+0x80+4*(TIMER_IRQ/32)) &= ~(1U<<(TIMER_IRQ%32));
  REG(GICD+0x400+4*(TIMER_IRQ/4)) = 0x80808080;
  if(TIMER_IRQ>=32) REG(GICD+0x800+4*(TIMER_IRQ/4)) = 0x01010101;
  REG(GICD+0x100+4*(TIMER_IRQ/32)) = 1U<<(TIMER_IRQ%32);
  REG(GICC+4)=0xff; REG(GICC+8)=0; REG(GICD)=1; REG(GICC)=1; dsb();
}
static void timer_arm(void) {
#ifdef T113_QEMU
  uint n=r_cntfrq_el0()/100, one=1;
  asm volatile("mcr p15, 0, %0, c14, c2, 0"::"r"(n));
  asm volatile("mcr p15, 0, %0, c14, c2, 1"::"r"(one));
#else
  REG(TIMER+4)=1;
#endif
}
static void timer_start(void) {
#ifdef T113_QEMU
  timer_arm();
#else
  REG(TIMER+0x10)=0; REG(TIMER)=0; REG(TIMER+4)=3;
  REG(TIMER+0x14)=240000; /* 24 MHz / 100 = 10 ms */
  REG(TIMER+0x10)=(1<<2)|(1<<1)|1;
  REG(TIMER)=1;
#endif
}
void t113_main(void) {
  uartinit();
  uartputc_sync('\r'); uartputc_sync('\n');
  kinit1(end,P2V(PHYSTOP)); kvminit(); kvminithart();
  device_init(); consoleinit(); ttyinit(); printfinit();
  printf("\nxv6 ARM32 / 100ask T113 Pro\n");
  printf("single CPU, ARMv7 MMU, embedded RAM disk\n");
  procinit(); initlock(&tickslock,"ticks");
  binit(); iinit(); fileinit(); userinit();
  interrupts_init(); timer_start();
  scheduler();
}
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
  uint iar=REG(GICC+12), irq=iar&1023;
  if(irq==TIMER_IRQ) {
    timer_arm();
    static uint sub;
    if(++sub==10) { sub=0; acquire(&tickslock); ticks++; wakeup(&ticks); release(&tickslock); }
    uartintr(); sched_tick();
  }
  if(irq<1020) REG(GICC+16)=iar;
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
