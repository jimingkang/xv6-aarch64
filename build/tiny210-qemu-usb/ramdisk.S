.section .ramdisk,"aw"
.global ramdisk_start,ramdisk_end
ramdisk_start:
.incbin "build/tiny210-qemu-usb/fs.img"
ramdisk_end:
.section .note.GNU-stack,"",%progbits
