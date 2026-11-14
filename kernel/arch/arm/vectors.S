.syntax unified
.arm
.text
.balign 32
.global vectors
vectors:
  b .
  b undef_entry
  b svc_entry
  b prefetch_entry
  b abort_entry
  b .
  b irq_entry
  b .
/* Keep the shared 64-bit trapframe layout. ARM registers occupy low words.
   All high words are cleared, so C can safely inspect uint64 arguments. */
.macro save_tf
  sub sp, sp, #272
  str r0, [sp, #0]
  str r1, [sp, #8]
  str r2, [sp, #16]
  str r3, [sp, #24]
  str r4, [sp, #32]
  str r5, [sp, #40]
  str r6, [sp, #48]
  str r7, [sp, #56]
  str r8, [sp, #64]
  str r9, [sp, #72]
  str r10, [sp, #80]
  str r11, [sp, #88]
  str r12, [sp, #96]
  str lr, [sp, #248]
  mrs r1, spsr
  str r1, [sp, #256]
  add r0, sp, #264
  stmia r0, {sp,lr}^
  nop
  ldr r1, [sp, #268]
  str r1, [sp, #240]
  mov r0, #0
  mov r1, #4
1: str r0, [sp, r1]
  add r1, r1, #8
  cmp r1, #272
  blo 1b
.endm
svc_entry:
  save_tf
  mov r0, sp
  bl t113_svc
  b restore_tf
/* IRQ uses its banked stack for a temporary frame. Switch to SVC before
   calling C, so sleep/yield can save and restore the process kernel stack. */
irq_entry:
  sub lr, lr, #4
  sub sp, sp, #72
  stmia sp, {r0-r12}
  str lr, [sp, #60]
  mrs r0, spsr
  str r0, [sp, #64]
  mov r0, sp
  add sp, sp, #72
  cps #0x13
  sub sp, sp, #272
  mov r1, #0
2: ldr r2, [r0, r1, lsl #2]
  str r2, [sp, r1, lsl #3]
  add r1, r1, #1
  cmp r1, #13
  blo 2b
  ldr r2, [r0, #60]
  str r2, [sp, #248]
  ldr r2, [r0, #64]
  str r2, [sp, #256]
  and r3, r2, #31
  cmp r3, #16
  bne 3f
  add r1, sp, #264
  stmia r1, {sp,lr}^
  nop
  ldr r2, [sp, #268]
  str r2, [sp, #240]
  b 4f
3: add r2, sp, #272
  str r2, [sp, #264]
  str lr, [sp, #240]
4: mov r1, #4
  mov r2, #0
5: str r2, [sp, r1]
  add r1, r1, #8
  cmp r1, #272
  blo 5b
  mov r0, sp
  bl t113_irq
  b restore_tf
.global usertrapret
usertrapret:
  mov sp, r0
restore_tf:
  ldr r0, [sp, #256]
  msr spsr_cxsf, r0
  and r0, r0, #31
  cmp r0, #16
  bne kernel_return
  ldr r0, [sp, #240]
  str r0, [sp, #268]
  add r0, sp, #264
  ldmia r0, {sp,lr}^
  nop
  ldr lr, [sp, #248]
  b load_regs
kernel_return:
  /* RFE restores kernel sp, PC and CPSR without destroying kernel LR. */
  ldr r0, [sp, #248]
  str r0, [sp, #264]
  ldr r0, [sp, #256]
  str r0, [sp, #268]
  ldr lr, [sp, #240]
load_regs:
  ldr r0, [sp, #0]
  ldr r1, [sp, #8]
  ldr r2, [sp, #16]
  ldr r3, [sp, #24]
  ldr r4, [sp, #32]
  ldr r5, [sp, #40]
  ldr r6, [sp, #48]
  ldr r7, [sp, #56]
  ldr r8, [sp, #64]
  ldr r9, [sp, #72]
  ldr r10, [sp, #80]
  ldr r11, [sp, #88]
  ldr r12, [sp, #96]
  /* Branch decision must not clobber a restored user register. */
  push {r0}
  mrs r0, spsr
  and r0, r0, #31
  cmp r0, #16
  pop {r0}
  add sp, sp, #264
  beq user_return
  rfeia sp!
user_return:
  add sp, sp, #8
  movs pc, lr
undef_entry:
  mov r0, #1
  b fault_entry
prefetch_entry:
  mov r0, #2
  b fault_entry
abort_entry:
  mov r0, #3
fault_entry:
  mov r1, lr
  mrs r2, spsr
  and r3, r2, #31
  cmp r3, #16
  bne 6f
  cps #0x13
6: bl t113_fault
  b .

.section .note.GNU-stack,"",%progbits
