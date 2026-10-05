.syntax unified
.arm
.section .text.entry,"ax"
.global _entry
_entry:
  cpsid if
  mrc p15, 0, r0, c0, c0, 5
  ands r0, r0, #255
  bne parked
  /* Some U-Boot variants enter in HYP. Drop to SVC before using banked
     exception stacks and disable inherited stage-2 virtualization. */
  mrs r0, cpsr
  and r0, r0, #31
  cmp r0, #26
  bne svc_start
  mov r0, #0
  mcr p15, 4, r0, c1, c1, 0
  mcr p15, 4, r0, c1, c1, 3
  mov r0, #3
  mcr p15, 4, r0, c14, c1, 0
  mov r0, #0xd3
  msr spsr_cxsf, r0
  adr r0, svc_start
  msr elr_hyp, r0
  eret
svc_start:
  /* go runs in privileged ARM state. Refuse a non-identity MMU handoff:
     the documented U-Boot dcache off command must disable translation. */
  mrc p15, 0, r0, c1, c0, 0
  tst r0, #1
  bne .
  bic r0, r0, #(1 << 12)
  bic r0, r0, #(1 << 2)
  bic r0, r0, #(1 << 13)
  bic r0, r0, #(1 << 1)
  orr r0, r0, #(1 << 23) /* extended page table format */
  mcr p15, 0, r0, c1, c0, 0
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
  bl t113_main
parked:
  wfi
  b parked
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
