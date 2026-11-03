OUTPUT_ARCH(riscv)
ENTRY(_start)
SECTIONS {
  . = 0x80000000;
  .text.init : { KEEP(*(.text.init)) }
  . = ALIGN(0x1000);
  .tohost : { *(.tohost) }
  . = ALIGN(0x1000);
  .text : { *(.text .text.*) }
  .rodata : { *(.rodata .rodata.*) }
  .data : { *(.data .data.*) }
  .got : { *(.got .got.* .igot .igot.*) }
  .sdata : {
    __global_pointer$ = . + 0x800;
    *(.srodata*) *(.sdata .sdata.*)
  }
  .sbss : { *(.sbss .sbss.*) *(.scommon) }
  .bss : { *(.bss .bss.*) *(COMMON) }
  .tdata : { _tdata_begin = .; *(.tdata .tdata.*); _tdata_end = .; }
  .tbss : { *(.tbss .tbss.*); _tbss_end = .; }
  _end = .;
  /* F's crt reserves a 16 MiB stack immediately beyond _end. */
  ASSERT(_end + 0x01000040 <= 0x88000000, "NPU image/heap/stack overlaps host DDR")
}
