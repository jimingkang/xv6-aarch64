.syntax unified
.arm
.text
.global swtch
swtch:
  str sp, [r0, #0]
  str r4, [r0, #8]
  str r5, [r0, #16]
  str r6, [r0, #24]
  str r7, [r0, #32]
  str r8, [r0, #40]
  str r9, [r0, #48]
  str r10, [r0, #56]
  str r11, [r0, #64]
  str lr, [r0, #104]
  ldr sp, [r1, #0]
  ldr r4, [r1, #8]
  ldr r5, [r1, #16]
  ldr r6, [r1, #24]
  ldr r7, [r1, #32]
  ldr r8, [r1, #40]
  ldr r9, [r1, #48]
  ldr r10, [r1, #56]
  ldr r11, [r1, #64]
  ldr lr, [r1, #104]
  bx lr

.section .note.GNU-stack,"",%progbits
