OUTPUT_ARCH( "aarch64" )
ENTRY( _entry )

SECTIONS
{
  /*
   * Raspberry Pi 3 loads the 64-bit kernel image at physical 0x80000.
   */
/*
  . = 0x40000000;
  */
  . = 0xffffff8000080000;

  .text : AT(0x80000) {
    *(.text .text.*)
    . = ALIGN(0x1000);
    PROVIDE(etext = .);
  }

  .rodata : {
    . = ALIGN(16);
    *(.srodata .srodata.*) /* do not need to distinguish this from .rodata */
    . = ALIGN(16);
    *(.rodata .rodata.*)
  }

  .data : {
    . = ALIGN(16);
    *(.sdata .sdata.*) /* do not need to distinguish this from .data */
    . = ALIGN(16);
    *(.data .data.*)
  }

  .bss : {
    . = ALIGN(16);
    PROVIDE(bss_start = .);
    *(.sbss .sbss.*) /* do not need to distinguish this from .bss */
    . = ALIGN(16);
    *(.bss .bss.*)
    . = ALIGN(16);
    PROVIDE(bss_end = .);
  }

  PROVIDE(end = .);
}

bss_size = (bss_end - bss_start) >> 3;
