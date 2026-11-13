/* KERNEL_LOAD is passed by the Makefile (--defsym): 0x30008000 on the X210,
   the address its U-Boot loads the kernel to (`movi read kernel 30008000`). */
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
  /* The X210 U-Boot runs at DRAM_BASE + 0x03E00000 while it loads us; the
     loaded image (not BSS, zeroed after the handoff) must stay below it. */
  ASSERT(image_end < KERNEL_LOAD - 0x8000 + 0x03E00000, "kernel image overlaps U-Boot")
}
