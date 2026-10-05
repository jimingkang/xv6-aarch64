.syntax unified
.arm
.text
.global __aeabi_uldivmod
__aeabi_uldivmod:
  push {r4,lr}
  sub sp, sp, #16
  add r4, sp, #8
  str r4, [sp]
  bl t113_udiv
  ldr r2, [sp, #8]
  ldr r3, [sp, #12]
  add sp, sp, #16
  pop {r4,pc}

.section .note.GNU-stack,"",%progbits
