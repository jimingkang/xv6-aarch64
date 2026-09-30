#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "aarch64.h"
#include "defs.h"
#include "device.h"

volatile static int started = 0;
extern char end[];  // first address after kernel loaded from ELF file

void _entry(void);
void delay(uint32 c);
void boot_uart_mark(int c);

// start() jumps here in EL1 on all CPUs.
void
main()
{
  boot_uart_mark('0');
  if(cpuid() == 0){
    // QEMU's Pi firmware parks secondary cores. This first port runs core 0.
    kinit1(end, P2V(EARLYTOP));  // memory covered by the bootstrap map
    boot_uart_mark('1');
    kvminit();       // create kernel page table
    boot_uart_mark('2');
    kvminithart();   // turn on paging
    boot_uart_mark('3');
    kinit2(P2V(EARLYTOP), P2V(PHYSTOP));
    boot_uart_mark('4');
    device_init();
    sdio_bus_init();
    usb_bus_init();
    consoleinit();
    ttyinit();
    printfinit();
    printf("\n");
    printf("xv6 kernel is booting\n");
    printf("build: bcm43430-sdio-v2\n");
    printf("\n");
    procinit();      // process table
    rnginit();       // BCM2837 hardware random source for cryptographic keys
    syncinit();      // process synchronization objects
    netinit();       // Ethernet/IPv4/UDP stack (loopback until NIC attaches)
    dwc2_driver_init();   // register/probe DWC2 USB host controller
    trapinit();      // trap vectors
    trapinithart();  // install trap vector
    gicv3init();     // set up interrupt controller
    gicv3inithart();
    timerinit();
    binit();         // buffer cache
    iinit();         // inode table
    fileinit();      // file table
    sd_driver_init(); // BCM SDHOST owns the external SD memory card
    arasan_sdio_driver_init(); // Arasan owns BCM43430/43455 on GPIO34--39
    fat32init();      // locate FS.IMG in a FAT32 boot partition
    brcmfmac_driver_init(); // firmware source is available after FAT32 init
    mt7601u_driver_init(); // bind enumerated USB MT7601U after firmware source
    ext2init();       // optional read-only Linux ext2 partition
    vfsinit();        // mount non-native filesystems behind vnode operations
    userinit();      // first user process
    // kvmdump();    // enable to show the final kernel page table before init/sh
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
