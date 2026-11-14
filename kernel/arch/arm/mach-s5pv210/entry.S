/* 九鼎 X210 (S5PV210, Cortex-A8) entry, following the standard X210 boot
   chain and the ARM Linux boot protocol that its U-Boot implements:
     iROM (BL0) -> BL1 (SD sector 1, 16 KiB, header + checksum, runs in
     iRAM at 0xD0020010: clocks, DDR) -> BL2 = U-Boot (SD sector 49,
     runs from DDR) -> `movi read kernel 30008000; bootm 30008000`
   On entry: SVC mode, IRQ/FIQ masked, MMU and D-cache off (U-Boot's
   cleanup_before_linux), I-cache may be on,
     r0 = 0, r1 = machine type (board.h BOARD_MACH),
     r2 = ATAG list (DRAM+0x100) or a DTB.
   The first 0x34 bytes form an ARM zImage header, so the raw image also
   boots through the Samsung U-Boot "Boot with zImage" path and `bootz`.
   Neither relocates: the image must sit at its link address. */
.syntax unified
.arm
.section .text.entry,"ax"
.global _entry
_entry:
  .rept 8
  mov r0, r0
  .endr
  b reset                 @ 0x20
  .word 0x016f2818        @ 0x24 zImage magic
  .word 0                 @ 0x28 start offset
  .word zimage_size       @ 0x2c end offset (image size)
  .word 0x04030201        @ 0x30 little-endian marker
reset:
  cpsid if
  mov r8, r0
  mov r9, r1
  mov r10, r2
  /* Running at the wrong address means a wrong load address. */
  adr r3, _entry
  ldr r4, =_entry
  cmp r3, r4
  adrne r0, msg_addr
  bne early_die
  /* Cortex-A8 has no HYP mode; insist on a privileged mode and use SVC. */
  mrs r0, cpsr
  and r0, r0, #31
  cmp r0, #16
  ldreq r0, =msg_mode     @ load address already verified
  beq early_die
  cps #0x13
  /* bootm turns translation off. The X210 vendor U-Boot runs with its MMU
     on, so a plain `go` arrives here with the MMU still enabled. */
  mrc p15, 0, r0, c1, c0, 0
  tst r0, #1
  ldrne r0, =msg_mmu
  bne early_die
  bic r0, r0, #(1 << 12)  @ I-cache off (caches stay off in this bring-up)
  bic r0, r0, #(1 << 2)   @ D-cache off
  bic r0, r0, #(1 << 13)  @ low vectors, VBAR used
  bic r0, r0, #(1 << 24)  @ VE: take IRQs at the 0x18 vector, not from the VIC
  bic r0, r0, #(1 << 1)   @ no alignment fault checking
  mcr p15, 0, r0, c1, c0, 0
  mov r0, #0
  mcr p15, 0, r0, c7, c5, 0  @ invalidate I-cache and branch predictor
  dsb
  isb
  cps #0x12
  ldr sp, =irq_stack_top
  cps #0x17
  ldr sp, =abort_stack_top
  cps #0x1b
  ldr sp, =undef_stack_top
  cps #0x13
  ldr sp, =boot_stack_top
  ldr r0, =bss_start
  ldr r1, =bss_end
  mov r2, #0
1: cmp r0, r1
  strlo r2, [r0], #4
  blo 1b
  ldr r0, =vectors
  mcr p15, 0, r0, c12, c0, 0
  isb
  mov r0, r8
  mov r1, r9
  mov r2, r10
  bl x210_main
parked:
  wfi
  b parked

/* Position-independent polled write to the console UART. */
early_die:
  ldr r1, =EARLY_UART
2: ldrb r2, [r0], #1
  cmp r2, #0
  beq parked
#ifdef X210_QEMU
3: ldr r3, [r1, #0x14]    @ 16550 LSR.THRE
  tst r3, #0x20
  beq 3b
  str r2, [r1]
#else
3: ldr r3, [r1, #0x10]    @ S5PV210 UTRSTAT.transmitter empty
  tst r3, #0x4
  beq 3b
  str r2, [r1, #0x20]     @ UTXH
#endif
  b 2b
msg_addr: .asciz "\r\nxv6: wrong load address; load the uImage at its header address (DRAM+0x8000)\r\n"
msg_mode: .asciz "\r\nxv6: entered in USR mode; boot with bootm\r\n"
msg_mmu:  .asciz "\r\nxv6: MMU still on; boot with bootm, not go\r\n"
.balign 4
.ltorg

.section .bss
.balign 16
.space 8192
boot_stack_top:
.space 4096
irq_stack_top:
.space 4096
abort_stack_top:
.space 4096
undef_stack_top:

.section .note.GNU-stack,"",%progbits
