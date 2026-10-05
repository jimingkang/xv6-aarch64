OUTPUT_ARCH(arm)
ENTRY(_entry)
SECTIONS {
  . = 0x40200000;
  .text : { KEEP(*(.text.entry)) *(.text .text.*) }
  . = ALIGN(4096);
  etext = .;
  .rodata : { *(.rodata .rodata.*) }
  .data : { *(.data .data.*) }
  . = ALIGN(4096);
  .ramdisk : { KEEP(*(.ramdisk)) }
  . = ALIGN(16);
  bss_start = .;
  .bss (NOLOAD) : { *(.bss .bss.* COMMON) }
  . = ALIGN(4096);
  bss_end = .;
  end = .;
  /DISCARD/ : { *(.ARM.exidx* .ARM.extab* .comment .note*) }
  ASSERT(end < 0x44000000, "kernel exceeds reserved DRAM")
}
