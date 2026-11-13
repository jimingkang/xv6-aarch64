/* 九鼎 X210 (Samsung S5PV210, Cortex-A8) physical layout.
   BL1/U-Boot have already initialised DMC0/DMC1. The X210 U-Boot maps
   DRAM at 0x30000000 (bank 1) and 0x40000000 (bank 2), puts ATAGs at
   0x30000100, and loads the kernel at 0x30008000. The kernel is linked at
   that address and identity mapped; ATAG_MEM gives the usable DRAM top. */
#ifdef X210_QEMU
/* QEMU has no S5PV210 machine. The test build runs on the Cortex-A8
   "cubieboard" model whose DRAM starts at 0x40000000. */
#define DRAM_BASE 0x40000000UL
#else
#ifndef X210_DRAM_BASE
#define X210_DRAM_BASE 0x30000000
#endif
#define DRAM_BASE ((unsigned long)X210_DRAM_BASE)
#endif
#define KERNBASE 0UL
#define KERNPA (DRAM_BASE + 0x8000UL)   /* Linux TEXT_OFFSET convention */
#define KERNLINK KERNPA
#define EXTMEM DRAM_BASE
#ifndef __ASSEMBLER__
extern unsigned int x210_phystop;
#endif
#define PHYSTOP ((uintptr)x210_phystop)
#define EARLYTOP PHYSTOP
/* User space is [0, MAXVA). DRAM starts at 0x20000000 or 0x30000000 on
   S5PV210 boards, so the user window must end below it. */
#define MAXVA 0x20000000UL
#define V2P(a) ((uintptr)(a))
#define P2V(a) ((void*)(uintptr)(a))
/* Unused by the ARM32 kernel-stack allocator (stacks are kalloc pages). */
#define KSTACK(p) (0xC0000000UL + (p)*8192UL)
#define UART0_PA 0xE2900800UL            /* UART2: X210 debug console */
#define UART0_IRQ 44                     /* VIC1 bit 12 (unused: polled) */
#define PERIPHERAL_BASE_PA 0xE0000000UL
#define TIMER0_IRQ 25                    /* VIC0 bit 25: PWM timer 4 */
#define ARM32_UART_NAME "s5pv210-uart2"
