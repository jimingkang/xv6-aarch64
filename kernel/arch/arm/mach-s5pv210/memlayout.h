/* S5PV210 (Cortex-A8) boards: FriendlyARM Tiny210, 九鼎 X210.
   BL1 (ports/tiny210/bl1) or U-Boot has already initialised DDR, put ATAGs at
   DRAM+0x100 and loaded the kernel at DRAM+0x8000 (0x20008000 on Tiny210,
   0x30008000 on X210). The kernel is linked there and identity mapped;
   ATAG_MEM gives the usable DRAM top. */
#ifndef TINY210_QEMU
#include "board.h"
#endif
#ifdef TINY210_QEMU
/* QEMU has no S5PV210 machine. The test build runs on the Cortex-A8
   "cubieboard" model whose DRAM starts at 0x40000000. */
#define DRAM_BASE 0x40000000UL
#else
#define DRAM_BASE ((unsigned long)BOARD_DRAM_BASE)
#endif
#define KERNBASE 0UL
#define KERNPA (DRAM_BASE + 0x8000UL)   /* Linux TEXT_OFFSET convention */
#define KERNLINK KERNPA
#define EXTMEM DRAM_BASE
#ifndef __ASSEMBLER__
extern unsigned int tiny210_phystop;
#endif
#define PHYSTOP ((uintptr)tiny210_phystop)
#define EARLYTOP PHYSTOP
/* User space is [0, MAXVA). DRAM starts at 0x20000000 or 0x30000000 on
   S5PV210 boards, so the user window must end below it. */
#define MAXVA 0x20000000UL
#define V2P(a) ((uintptr)(a))
#define P2V(a) ((void*)(uintptr)(a))
/* Unused by the ARM32 kernel-stack allocator (stacks are kalloc pages). */
#define KSTACK(p) (0xC0000000UL + (p)*8192UL)
#ifdef TINY210_QEMU
#define UART0_PA 0x01C28000UL
#else
#define UART0_PA ((unsigned long)BOARD_UART)  /* board console UART */
#endif
#define UART0_IRQ 42                     /* VIC1 bit 10+n (unused: polled) */
#define PERIPHERAL_BASE_PA 0xE0000000UL
#define TIMER0_IRQ 25                    /* VIC0 bit 25: PWM timer 4 */
#define ARM32_UART_NAME "console"   /* kernel/console.c device; the UART driver is serial/samsung.c */
/* LCD frame buffer: just above the memory handed to the page allocator. */
#define S5PV210_USE_DRAM  (128U<<20)
#define S5PV210_FB_OFFSET (128U<<20)
#define S5PV210_FB_SIZE   (2U<<20)
