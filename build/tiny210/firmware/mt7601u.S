.section .rodata
.balign 4
.global fw_mt7601u_start,fw_mt7601u_end
fw_mt7601u_start:
.incbin "firmware/mt7601u.bin"
fw_mt7601u_end:
.section .note.GNU-stack,"",%progbits
