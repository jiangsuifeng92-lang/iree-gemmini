.syntax unified
.arch armv4
.arm
.section .vectors, "ax"
.global _start
_start:
    b reset
    b fault
    b fault
    b fault
    b fault
    b fault
    b fault
    b fault
.section .text.start, "ax"
reset:
    msr cpsr_c, #0xd3
    ldr sp, =__stack_top
    ldr r0, =__bss_start
    ldr r1, =__bss_end
    mov r2, #0
1:  cmp r0, r1
    strlo r2, [r0], #4
    blo 1b
    bl host_main
2:  b 2b
fault:
    mov r0, #0x10000000
    mov r1, #2
    str r1, [r0]
3:  b 3b
