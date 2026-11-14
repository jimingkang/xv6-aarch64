/* xv6 BL1 for the 九鼎 X210 (S5PV210): replaces U-Boot.
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
  ldr r0, =bl1_bss_start
  ldr r1, =bl1_bss_end
  mov r2, #0
1: cmp r0, r1
  strlo r2, [r0], #4
  blo 1b
  bl bl1_main
2: wfi
  b 2b

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
