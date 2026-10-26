#include <kernel/types.h>
#include <kernel/param.h>
#include "memlayout.h"
#include "aarch64.h"
#include <kernel/spinlock.h>
#include <kernel/proc.h>
#include <kernel/defs.h>
#include <kernel/mm.h>
/*如果中断发生在进程的内核上下文中，
 比如：中断发生在进程执行系统调用期间，
usertrap -> syscall -> sys_write -> filewrite ...
那它用的就是：当前进程 p 的内核栈
-----------------
低地址
p->kstack
  |
  |  普通内核栈空间
  |
  |  +-------------------------  <- 中断后 sp = S - 272
  |  | el1irq savereg 临时现场 |
  |  | x0..x30, elr, spsr, sp  |
  |  +-------------------------  <- 中断前 sp = S
  |
  |  函数调用帧、局部变量、返回地址等
  |
  +-------------------------
  |  struct trapframe       <- p->trapframe
  |  x0..x30
  |  elr
  |  spsr
  |  用户 sp
  +-------------------------
p->kstack + PGSIZE
高地址
-------------------
*/
struct spinlock tickslock;
uint ticks;

// in trapvec.S, calls kerneltrap() or usertrap().
void alltraps();

extern int devintr();

void
trapinit(void)
{
  initlock(&tickslock, "time");
}

// set up to take exceptions and traps.
void
trapinithart(void)
{
  w_vbar_el1((uint64)alltraps);
}

//
// handle an interrupt, exception, or system call from user space.
// called from uservec.S
//

void
userirq(void)
{
  struct proc *p = myproc();

  devintr();
  // Give up the CPU if the scheduler asked for it: slice used up, or a
  // higher-priority process became runnable (timer tick or IPI).
  if(resched_pending())
    yield();

  if(p->killed)
    exit(-1);

  intr_off();
}

// EL0 data abort (EC 0x24) or instruction abort (EC 0x20).  Translation
// (DFSC 0b0001ll), access-flag (0b0010ll) and permission (0b0011ll) faults
// may be demand paging of an mmap() region; anything else, or an address no
// VMA allows, falls through to the "unexpected" report and kills the
// process (SIGSEGV/SIGBUS in Linux terms).
static int
user_page_fault(struct proc *p, uint64 esr)
{
  uint64 far = r_far_el1();
  uint64 iss = esr & 0x1ffffff;
  uint64 fsc = iss & 0x3f;
  int flags = 0, r;

  if(fsc < 0x04 || fsc > 0x0f || far < mmap_lowest(p->vm))
    return 0;                 // not an mmap() region: keep the full report
  if(((esr >> 26) & 0x3f) == 0x20)
    flags |= FAULT_FLAG_INSTR;
  else if((iss & (1 << 6)) && !(iss & (1 << 8)))   // WnR, not cache maint.
    flags |= FAULT_FLAG_WRITE;
  intr_on();                  // reading a file page may sleep
  r = handle_mm_fault(p->vm, far, flags);
  if(r == VM_FAULT_OK)
    return 1;
  printf("pid %d %s: %s at va=%p pc=%p\n", p->pid, p->name,
         r == VM_FAULT_SIGBUS ? "SIGBUS" : r == VM_FAULT_OOM ?
         "out of memory" : "SIGSEGV", far, p->trapframe->elr);
  p->killed = 1;
  return 1;
}

void
usertrap(void)
{
  struct proc *p = myproc();

  uint64 esr = r_esr_el1();
  uint64 ec = (esr >> 26) & 0x3f;
  w_esr_el1(0);
  if(ec == 21){
    // system call

    if(p->killed)
      exit(-1);

    intr_on();

    syscall();
    // The syscall may have woken a higher-priority process for this CPU.
    if(resched_pending())
      yield();
  } else if((ec == 0x24 || ec == 0x20) && user_page_fault(p, esr)){
    // Data/instruction abort from EL0 satisfied by an mmap() VMA.
  } else {
    printf("usertrap(): unexpected esr=%p ec=%p pid=%d\n", esr, ec, p->pid);
    printf("            elr=%p far=%p\n", r_elr_el1(), r_far_el1());
    printf("            sz=%p sp=%p x0=%p x1=%p x2=%p x3=%p\n",
           p->vm->sz, p->trapframe->sp, p->trapframe->x0,
           p->trapframe->x1, p->trapframe->x2, p->trapframe->x3);
    uvmdump(p->vm->pagetable, p->pid, p->name, "usertrap");
    p->killed = 1;
  }

  if(p->killed)
    exit(-1);

  intr_off();
}


// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is.
void 
kerneltrap()
{
  uint64 esr = r_esr_el1();
  w_esr_el1(0);
  
  if(intr_get() != 0)
    panic("kerneltrap: interrupts enabled");

  uint64 ec = (esr >> 26) & 0x3f;
  uint64 iss = esr & 0x1ffffff;
  printf("esr %p %p %p\n", esr, ec, iss);
  printf("elr=%p far=%p\n", r_elr_el1(), r_far_el1());
  panic("kerneltrap");
}

void
kernelirq()
{
  devintr();

  // Preempt kernel code too (interrupts are only enabled when no spinlock is
  // held), so a woken real-time task does not wait for a long syscall.
  if(myproc() != 0 && myproc()->state == RUNNING && resched_pending())
    yield();
}


void
clockintr()
{
  acquire(&tickslock);
  ticks++;
  wakeup(&ticks);
  release(&tickslock);
  // Timed UDP receivers normally wake immediately from udp_rx().  This
  // periodic wake is only for checking an expired receive deadline.
  net_udp_timeout_tick();
}

// check if it's an external interrupt and handle it.
// returns 2 if timer interrupt,
// 1 if other device,
// 0 if not recognized.
int
devintr()
{
  uint32 iar = gic_iar();
  uint32 irq = gic_iar_irq(iar);
  int dev = 0;

  // printf("devintr irq %d", irq);
  if(irq == UART0_IRQ){
    uartintr();
    dev = 1;
  } else if(irq == DWC2_IRQ){
    dwc2_irq();
    dev = 1;
  } else if(irq == SDIO_IRQ){
    arasan_sdio_irq();
    dev = 1;
  } else if(irq == CSI1_IRQ){
    unicam_irq();
    dev = 1;
  } else if(irq == TIMER0_IRQ){
    int logical_tick = timerintr();
    if(cpuid() == 0 && logical_tick){
      clockintr();
    }
    // Every 10 ms interrupt is a scheduler tick (time slices, RT throttling).
    // xv6 `ticks` used by sleep()/uptime() still advance every 100 ms.
    sched_tick();
    dev = 2;
  } else if(irq == IPI_RESCHED_IRQ){
    resched_ipi_ack();          // need_resched was set by the sender
    dev = 1;
  } else if(irq == 1023){
    // do nothing
  } else if(irq){
    printf("unexpected interrupt irq=%d\n", irq);
  }

  if(dev) {
    gic_eoi(iar);
  }

  return dev;
}
