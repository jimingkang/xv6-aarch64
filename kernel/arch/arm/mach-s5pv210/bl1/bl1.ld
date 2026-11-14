/* BL1 runs in iRAM. The iROM loads 16 KiB to 0xD0020000; the first 16
   bytes are the header, code starts at 0xD0020010. BSS and the stack use
   the rest of iRAM below the iROM's own data at 0xD0037xxx. */
OUTPUT_ARCH(arm)
ENTRY(_start)
BL1_STACK_TOP = 0xD0036000;
SECTIONS {
  . = 0xD0020010;
  bl1_start = .;
  .text : { KEEP(*(.text.start)) *(.text .text.*) }
  .vectors : ALIGN(32) { KEEP(*(.bl1vectors)) }
  .rodata : { *(.rodata .rodata.*) }
  .data : { *(.data .data.*) }
  bl1_end = .;
  . = ALIGN(4);
  bl1_bss_start = .;
  .bss (NOLOAD) : { *(.bss .bss.* COMMON) }
  . = ALIGN(4);
  bl1_bss_end = .;
  /DISCARD/ : { *(.ARM.exidx* .ARM.extab* .comment .note*) }
  ASSERT(ADDR(.text) == 0xD0020010, "BL1 entry must be at 0xD0020010 (iROM jumps there)")
  ASSERT(bl1_end - bl1_start <= 16368, "BL1 larger than 16 KiB - 16 byte header")
  ASSERT(bl1_bss_end < BL1_STACK_TOP - 0x1000, "BL1 BSS runs into its stack")
}
