/* TI AM335x (BeagleBone Black) physical layout.
   U-Boot/SPL have already initialised EMIF0 DDR3 at 0x80000000.
   The kernel is linked at the BBB U-Boot ${loadaddr} (0x82000000) and is
   identity mapped. The usable DRAM top comes from the DTB /memory node. */
#ifdef AM335X_QEMU
/* QEMU has no AM335x machine. The test build runs on the Cortex-A8
   "cubieboard" model whose DRAM starts at 0x40000000. */
#define DRAM_BASE 0x40000000UL
#else
#define DRAM_BASE 0x80000000UL
#endif
#define KERNBASE 0UL
#define KERNPA (DRAM_BASE + 0x02000000UL)
#define KERNLINK KERNPA
#define EXTMEM DRAM_BASE
#ifndef __ASSEMBLER__
extern unsigned int am335x_phystop;
#endif
#define PHYSTOP ((uintptr)am335x_phystop)
#define EARLYTOP PHYSTOP
/* User space is [0, MAXVA); all DRAM and MMIO is above it on AM335x. */
#define MAXVA 0x40000000UL
#define V2P(a) ((uintptr)(a))
#define P2V(a) ((void*)(uintptr)(a))
/* Unused by the ARM32 kernel-stack allocator (stacks are kalloc pages). */
#define KSTACK(p) (0xC0000000UL + (p)*8192UL)
#define UART0_PA 0x44E09000UL
#define UART0_IRQ 72
#define PERIPHERAL_BASE_PA 0x44C00000UL
#define TIMER0_IRQ 68
#define ARM32_UART_NAME "am335x-uart0"
