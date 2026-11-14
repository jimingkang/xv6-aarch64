/* xv6 BL1 for the FriendlyARM Tiny210 (S5PV210): replaces U-Boot.
   The iROM (BL0) copies 16 KiB from SD sector 1 to iRAM 0xD0020000,
   checks the 16-byte header and branches here, to 0xD0020010, in SVC mode
   with the MMU off. bl1_main() brings up power hold, clocks, UART2 and
   DDR, copies xv6 from SD and calls bl1_boot(). */
.syntax unified
.arm
.section .text.start,"ax"
.global _start
_start:
  cpsid if
  ldr sp, =BL1_STACK_TOP
  ldr r0, =bl1_vectors            @ our own exception vectors (VBAR)
  mcr p15, 0, r0, c12, c0, 0
  mrc p15, 0, r0, c1, c0, 0
  bic r0, r0, #(1 << 13)          @ SCTLR.V = 0: vectors at VBAR
  bic r0, r0, #(1 << 1)           @ SCTLR.A = 0: the iROM leaves alignment checking on
  mcr p15, 0, r0, c1, c0, 0
  isb
  ldr r0, =bl1_bss_start
  ldr r1, =bl1_bss_end
  mov r2, #0
1: cmp r0, r1
  strlo r2, [r0], #4
  blo 1b
  bl bl1_main
2: wfi
  b 2b

/* VBAR needs a 32-byte aligned table. It lives in its own section placed
   after .text (bl1.ld): aligning .text.start would move the entry point
   off 0xD0020010, where the iROM jumps. */
.section .bl1vectors,"ax"
.balign 32
bl1_vectors:
  b .
  b 1f
  b 2f
  b 3f
  b 4f
  b .
  b 6f
  b 7f
1: mov r0, #1
  b vec_common
2: mov r0, #2
  b vec_common
3: mov r0, #3
  b vec_common
4: mov r0, #4
  b vec_common
6: mov r0, #6
  b vec_common
7: mov r0, #7
vec_common:
  mov r1, lr
  ldr sp, =BL1_STACK_TOP - 0x1000  @ banked mode sp: a scratch area below the main stack
  bl bl1_exception
8: b 8b
.ltorg

.section .text.start,"ax"
/* bl1_boot(entry, machid, atags): ARM Linux handoff, as bootm does it.
   SVC, IRQ/FIQ masked, MMU/D-cache off (never enabled here), I-cache off. */
.global bl1_boot
bl1_boot:
  mov r4, r0
  mov r5, r1
  mov r6, r2
  mrc p15, 0, r0, c1, c0, 0
  bic r0, r0, #(1 << 12)
  bic r0, r0, #(1 << 2)
  bic r0, r0, #1
  mcr p15, 0, r0, c1, c0, 0
  mov r0, #0
  mcr p15, 0, r0, c7, c5, 0  @ invalidate I-cache and branch predictor
  dsb
  isb
  mov r0, #0
  mov r1, r5
  mov r2, r6
  bx r4

.section .note.GNU-stack,"",%progbits
