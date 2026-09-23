#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "aarch64.h"
#include "defs.h"

volatile static int started = 0;
extern char end[];  // first address after kernel loaded from ELF file

void _entry(void);
void delay(uint32 c);

// start() jumps here in EL1 on all CPUs.
void
main()
{
  if(cpuid() == 0){
    // QEMU's Pi firmware parks secondary cores. This first port runs core 0.
    kinit1(end, P2V(EARLYTOP));  // memory covered by the bootstrap map
    kvminit();       // create kernel page table
    kvminithart();   // turn on paging
    kinit2(P2V(EARLYTOP), P2V(PHYSTOP));
    consoleinit();
    printfinit();
    printf("\n");
    printf("xv6 kernel is booting\n");
    printf("\n");
    procinit();      // process table
    trapinit();      // trap vectors
    trapinithart();  // install trap vector
    gicv3init();     // set up interrupt controller
    gicv3inithart();
    timerinit();
    binit();         // buffer cache
    iinit();         // inode table
    fileinit();      // file table
    ramdiskinit();      // fs.img loaded in RAM by QEMU
    userinit();      // first user process
    __sync_synchronize();
    started = 1;
  } else {
    while(started == 0)
      ;
    __sync_synchronize();
    kvminithart();    // turn on paging
    printf("hart %d starting\n", cpuid());
    trapinithart();   // install trap vector
    gicv3inithart();
    timerinit();
  }

  scheduler();
}
