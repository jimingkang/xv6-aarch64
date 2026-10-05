/* U-Boot supplies initialized DRAM; only the first 64 MiB are used. */
#define KERNBASE 0UL
#define KERNPA 0x40200000UL
#define KERNLINK KERNPA
#define EXTMEM 0x40000000UL
#define PHYSTOP 0x44000000UL
#define EARLYTOP PHYSTOP
#define MAXVA 0x40000000UL
#define V2P(a) ((uintptr)(a))
#define P2V(a) ((void*)(uintptr)(a))
#define KSTACK(p) (0x80000000UL + (p)*8192UL)
#define UART0_PA 0x02500000UL
#define UART0_IRQ 34
#define PERIPHERAL_BASE_PA 0x02000000UL
#define TIMER0_IRQ 91
