/* S5PV210 board selection, shared by bl1/ and the kernel.
   make S5PV210_BOARD=tiny210 (default) or S5PV210_BOARD=x210. */
#if defined(BOARD_X210)
/* 九鼎 X210: console UART2, DMC0 2 x 1 Gbit x16 = 256 MiB at 0x30000000. */
#define BOARD_NAME "Jiuding X210 (Samsung S5PV210)"
#define BOARD_DRAM_BASE 0x30000000
#define BOARD_DRAM_SIZE (256U<<20)
#define BOARD_MEMCONFIG0 0x30F01313   /* base 0x30, 256 MiB, 13 row, 10 col, 8 banks */
#define BOARD_UART 0xE2900800         /* UART2 */
#define BOARD_UART_N 2
#define BOARD_UART_GPIOCON 0xE0200020 /* GPA1CON: GPA1[0:1] = UART2 RXD/TXD */
#define BOARD_MACH 2456               /* SMDKV210, used by the X210 BSP */
#define BOARD_MACH_NAME "SMDKV210"
#elif defined(BOARD_TINY210)
/* FriendlyARM Tiny210 (V2 SDK): console UART0 (COM0 on the SDK base board),
   DMC0 4 x 1 Gbit x8 = 512 MiB at 0x20000000. */
#define BOARD_NAME "FriendlyARM Tiny210 (Samsung S5PV210)"
#define BOARD_DRAM_BASE 0x20000000
#define BOARD_DRAM_SIZE (512U<<20)
#define BOARD_MEMCONFIG0 0x20E01323   /* base 0x20, 512 MiB, 14 row, 10 col, 8 banks */
#define BOARD_UART 0xE2900000         /* UART0 */
#define BOARD_UART_N 0
#define BOARD_UART_GPIOCON 0xE0200000 /* GPA0CON: GPA0[0:1] = UART0 RXD/TXD */
#define BOARD_MACH 3466               /* MINI210, used by FriendlyARM's kernels */
#define BOARD_MACH_NAME "MINI210"
#else
#error "define BOARD_TINY210 or BOARD_X210"
#endif
