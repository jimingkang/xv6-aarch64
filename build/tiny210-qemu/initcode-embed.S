.section .rodata
.global initcode,initcode_end
initcode:
.incbin "build/tiny210-qemu/initcode.bin"
initcode_end:
.section .note.GNU-stack,"",%progbits
