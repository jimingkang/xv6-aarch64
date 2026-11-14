.syntax unified
.arm
#include "kernel/syscall.h"
.global start
start:
  adr r0, init
  adr r1, argv
  mov r7, #SYS_exec
  svc #0
  mov r0, #1
  mov r7, #SYS_exit
  svc #0
  b .
init: .asciz "/init"
.balign 4
argv: .word init, 0

.section .note.GNU-stack,"",%progbits
