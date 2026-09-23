// Physical memory layout

// QEMU Raspberry Pi 3B / BCM2837 physical layout used by this port.
//
// 00000000 -- RAM
// 00080000 -- QEMU loads the 64-bit kernel image here
// 07000000 -- in-memory fs.img loaded by QEMU
// 3f000000 -- BCM2837 peripheral window
// 40000000 -- ARM local peripherals

// the kernel uses physical memory thus:
// 00080000 -- entry.S, then kernel text and data
// end -- start of kernel page allocation area
// PHYSTOP -- end RAM used by the kernel

#define EXTMEM    0x00000000L
#define PHYSTOP   0x07000000L               // keep the ramdisk out of kalloc
#define KERNPA    0x00080000L
#define EARLYTOP  0x00200000L
#define RAMDISK_PA 0x07000000L
#define RAMDISK_SIZE (1024*1024L)

#define KERNBASE  0xffffff8000000000L     // First kernel virtual address
#define KERNLINK  (KERNBASE + KERNPA)     // virtual address where kernel is linked

#define V2P(a) (((uint64)(a)) - KERNBASE)
#define P2V(a) ((void *)(((char *)(a)) + KERNBASE))

#define V2P_WO(x) ((x) - KERNBASE)    // same as V2P, but without casts
#define P2V_WO(x) ((x) + KERNBASE)    // same as P2V, but without casts

// one beyond the highest possible virtual address.
#define MAXVA (KERNBASE + (1ULL<<38))

#define PERIPHERAL_BASE_PA 0x3f000000L
#define PERIPHERAL_BASE    (KERNBASE + PERIPHERAL_BASE_PA)
#define LOCAL_BASE_PA      0x40000000L
#define LOCAL_BASE         (KERNBASE + LOCAL_BASE_PA)

#define UART0       (PERIPHERAL_BASE + 0x201000L)
#define UART0_IRQ   57
#define TIMER0_IRQ  27

#define IRQCTRL     (PERIPHERAL_BASE + 0x00b000L)
#define RAMDISK     (KERNBASE + RAMDISK_PA)

// map kernel stacks beneath the trampoline,
// each surrounded by invalid guard pages.
#define KSTACK(p) (MAXVA - ((p)+1) * 2*PGSIZE)
