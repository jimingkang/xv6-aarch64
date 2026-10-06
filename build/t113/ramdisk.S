.section .ramdisk,"aw"
.global ramdisk_start,ramdisk_end
ramdisk_start:
.incbin "build/t113/fs.img"
ramdisk_end:
.section .note.GNU-stack,"",%progbits
