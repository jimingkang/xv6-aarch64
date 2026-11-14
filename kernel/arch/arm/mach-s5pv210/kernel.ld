/* KERNEL_LOAD is passed by the Makefile (--defsym): DRAM+0x8000 (0x20008000),
   where ports/tiny210/bl1 (or a U-Boot bootm) places the kernel. */
OUTPUT_ARCH(arm)
ENTRY(_entry)
SECTIONS {
  . = KERNEL_LOAD;
  image_start = .;
  .text : { KEEP(*(.text.entry)) *(.text .text.*) }
  . = ALIGN(4096);
  etext = .;
  .rodata : { *(.rodata .rodata.*) }
  .initcall.list : { __initcall_start = .; KEEP(*(.initcall.list)) __initcall_end = .; }
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
}
