/* Board constants shared by bl1/ and the kernel: FriendlyARM Tiny210 (V2
   SDK). Console UART0 (COM0 on the SDK base board); DMC0 DDR2, 4 x 1 Gbit x8
   = 512 MiB at 0x20000000. */
#define BOARD_NAME "FriendlyARM Tiny210 (Samsung S5PV210)"
#define BOARD_DRAM_BASE 0x20000000
#define BOARD_DRAM_SIZE (512U<<20)
#define BOARD_MEMCONFIG0 0x20E01323   /* base 0x20, 512 MiB, 14 row, 10 col, 8 banks */
#define BOARD_UART 0xE2900000         /* UART0 */
#define BOARD_UART_N 0
#define BOARD_UART_GPIOCON 0xE0200000 /* GPA0CON: GPA0[0:1] = UART0 RXD/TXD */
#define BOARD_MACH 3466               /* MINI210, used by FriendlyARM's kernels */
#define BOARD_MACH_NAME "MINI210"
