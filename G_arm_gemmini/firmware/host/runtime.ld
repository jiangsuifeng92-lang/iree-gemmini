ENTRY(_start)
MEMORY {
  BOOT (rx) : ORIGIN = 0, LENGTH = 64K
  HOST (rwx) : ORIGIN = 0x88000000, LENGTH = 16M
  MODEL (r) : ORIGIN = 0x89000000, LENGTH = 16M
}
SECTIONS {
  .boot : { KEEP(*(.vectors)) KEEP(*(.text.start)) } > BOOT
  .text : { *(.text .text.*) } > HOST
  .rodata : { *(.rodata .rodata.*) } > HOST
  .data : { *(.data .data.* .sdata .sdata.*) } > HOST
  /* IREE's archive contains PIC references. GOT must precede the heap. */
  .got : {
    __got_start = .;
    *(.got .got.* .igot .igot.*)
    __got_end = .;
  } > HOST
  .bss (NOLOAD) : {
    . = ALIGN(16); __bss_start = .;
    *(.bss .bss.* .sbss .sbss.*) *(COMMON)
    . = ALIGN(16); __bss_end = .;
  } > HOST
  __heap_start = ALIGN(__bss_end, 16);
  __heap_end = ORIGIN(HOST) + LENGTH(HOST) - 256K;
  /* Keep the Arm stack in its private 64 KiB on-chip boot RAM. */
  __stack_top = 0x10000;
  ASSERT(SIZEOF(.boot) <= 0x1000, "Arm boot code overlaps reserved stack space")
  .model : { KEEP(*(.model)) } > MODEL
  ASSERT(SIZEOF(.boot) <= 64K, "boot RAM overflow")
  ASSERT(__got_end <= __heap_start, "Arm GOT overlaps heap")
  ASSERT(__heap_start + 4M <= __heap_end, "Arm runtime needs at least 4 MiB heap")
  /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) *(.eh_frame*) }
}
