/* KERNEL_LOAD is passed by the Makefile (--defsym): 0x82000000 on the
   BeagleBone Black, i.e. the am335x_evm U-Boot ${loadaddr} used by bootz. */
OUTPUT_ARCH(arm)
ENTRY(_entry)
SECTIONS {
  . = KERNEL_LOAD;
  image_start = .;
  .text : { KEEP(*(.text.entry)) *(.text .text.*) }
  . = ALIGN(4096);
  etext = .;
  .rodata : { *(.rodata .rodata.*) }
  .data : { *(.data .data.*) }
  . = ALIGN(4096);
  .ramdisk : { KEEP(*(.ramdisk)) }
  . = ALIGN(16);
  image_end = .;
  bss_start = .;
  .bss (NOLOAD) : { *(.bss .bss.* COMMON) }
  . = ALIGN(4096);
  bss_end = .;
  end = .;
  /* zImage header "end" field: bytes U-Boot must reserve for the image. */
  zimage_size = image_end - image_start;
  /DISCARD/ : { *(.ARM.exidx* .ARM.extab* .comment .note*) }
  /* ${fdtaddr} is 0x88000000 on BBB (KERNEL_LOAD + 96 MiB): stay below it. */
  ASSERT(end < KERNEL_LOAD + 0x06000000, "kernel overlaps U-Boot fdtaddr")
}
