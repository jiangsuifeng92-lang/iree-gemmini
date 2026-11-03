ENTRY(_start)
MEMORY { HOST_RAM (rwx) : ORIGIN = 0, LENGTH = 64K }
SECTIONS {
  .vectors : { KEEP(*(.vectors)) } > HOST_RAM
  .text : { *(.text.start) *(.text .text.*) } > HOST_RAM
  .rodata : { *(.rodata .rodata.*) } > HOST_RAM
  .data : { *(.data .data.*) } > HOST_RAM
  .bss (NOLOAD) : {
    . = ALIGN(4); __bss_start = .;
    *(.bss .bss.*) *(COMMON)
    . = ALIGN(4); __bss_end = .;
  } > HOST_RAM
  __stack_top = ORIGIN(HOST_RAM) + LENGTH(HOST_RAM);
  ASSERT(__bss_end + 4096 <= __stack_top, "host firmware overlaps stack")
  /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) }
}
