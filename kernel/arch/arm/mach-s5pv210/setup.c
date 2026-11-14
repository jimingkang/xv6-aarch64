/* S5PV210 (Cortex-A8) board setup: FriendlyARM Tiny210 / 九鼎 X210
   (the role of Linux's arch/arm/mach-s5pv210 + setup_arch()).

   Entered from entry.S with the ARM Linux boot registers. Reads the ATAG
   list (or DTB), sizes DRAM, turns the MMU on (devices.c supplies the I/O
   mappings), registers the board's platform devices and runs the initcall
   table: the drivers under kernel/drivers/ (clock, watchdog, VIC, PWM
   timer, UART, FIMD) bind to those devices and bring the hardware up.
   Inherits from BL1/U-Boot: clocks, DDR2, console UART pinmux, 115200 8N1.

   X210_QEMU builds the same kernel for QEMU's Cortex-A8 "cubieboard"
   (Allwinner A10 peripherals, sun4i drivers), the only Cortex-A8 machine
   QEMU has. */
#include "kernel/types.h"
#include "kernel/param.h"
#include "kernel/memlayout.h"
#include "kernel/aarch64.h"
#include "kernel/spinlock.h"
#include "kernel/proc.h"
#include "kernel/defs.h"
#include "kernel/device.h"
#include "kernel/module.h"
#include "kernel/arch/arm/fdt.h"
#include "kernel/arch/arm/irq.h"
#include "kernel/arch/arm/time.h"
#include "atag.h"
#include "devices.h"
_Static_assert(sizeof(struct trapframe)==272, "ARM exception frame size");
_Static_assert(__builtin_offsetof(struct trapframe,x30)==240, "ARM user LR slot");
_Static_assert(__builtin_offsetof(struct trapframe,elr)==248, "ARM PC slot");
_Static_assert(__builtin_offsetof(struct context,x30)==104, "ARM context LR slot");
_Static_assert(DRAM_BASE>=MAXVA, "DRAM must lie above the user window");
#ifdef X210_QEMU
#define BOARD "QEMU cubieboard (Cortex-A8 stand-in for S5PV210)"
#else
#define BOARD BOARD_NAME
#endif
/* Caches stay off in this bring-up, and kinit junk-fills every free page
   uncached, so only the first USE_DRAM bytes of DDR are handed to the
   allocator (Tiny210 has 512 MiB; ATAG_MEM is still reported). */
#define USE_DRAM S5PV210_USE_DRAM
_Static_assert(S5PV210_FB_OFFSET>=USE_DRAM, "LCD frame buffer overlaps the page allocator");

extern char end[];
uint x210_phystop = DRAM_BASE + USE_DRAM;
static struct bootparams bootp;
static struct fdt_info bootfdt;

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
  /* The console UART works from the first instruction (serial driver's
     early base = the boot loader's console). */
  uartinit();
  uartputc_sync('\r'); uartputc_sync('\n');
#ifndef X210_QEMU
  /* MMU still off: iRAM is reachable; remember which SD channel booted. */
  s5pv210_note_boot_device(*(volatile uint32*)0xD0037488U);
#endif
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
  kvminit(); kvminithart();
  /* Instruction cache and branch prediction on. D-cache stays off (DMA
     buffers need no maintenance), but uncached instruction fetches were
     most of the DRAM traffic and starved the USB host's DMA. exec()
     invalidates the I-cache after loading a program (uvmsync_icache). */
  { uint z=0, v; asm volatile("mcr p15, 0, %0, c7, c5, 0"::"r"(z):"memory");
    asm volatile("mrc p15, 0, %0, c1, c0, 0":"=r"(v)); v |= (1U<<12)|(1U<<11);
    asm volatile("mcr p15, 0, %0, c1, c0, 0"::"r"(v):"memory"); isb(); }
  kinit1(end,P2V(PHYSTOP));
  device_init(); consoleinit(); ttyinit(); printfinit();
  printf("\nxv6 ARM32 / %s\n",BOARD);
  boot_info(r0,machid,r2);
  printf("kernel %x-%x, DRAM %x-%x\n",KERNPA,(uint)(uintptr)end,DRAM_BASE,x210_phystop);
  printf("single CPU, ARMv7 MMU, embedded RAM disk\n");
  arm32_time_init();
  procinit();              /* before the drivers: some start kernel threads */
  workqueue_init();
  netinit();
  s5pv210_register_devices();
  do_initcalls();          /* drivers probe; interrupts stay off until scheduler() */
#ifdef XV6_HAVE_VFS
  /* Root: FS.IMG in the SD card's FAT32 partition (or a raw xv6 partition),
     else the built-in RAM disk. The FAT32 volume itself is mounted at /boot
     by init. */
  fat32init();
  vfsinit();
#endif
  binit(); iinit(); fileinit(); userinit();
  scheduler();
}

/* Exception entry points called from kernel/arch/arm/vectors.S. */
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
  arch_handle_irq();       /* root irqchip driver -> request_irq() handlers */
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
