/*
 * Island entry and exit (built with the musl toolchain, linked into the island).
 *
 * lx_island_main(host, stack_lo, stack_size) saves what the host expects preserved (x19-x30, d8-d15, sp and
 * tpidr_el0, which musl takes over as its thread pointer) and continues in lx_island_start (C). The program's
 * exit_group on the main thread ends in lx_island_return(code), which restores that frame from whatever stack the
 * program is on and returns code to the host.
 */
    .text
    .global lx_island_main
    .type lx_island_main, %function
lx_island_main:
    adrp    x9, lx_host_frame
    add     x9, x9, :lo12:lx_host_frame
    stp     x19, x20, [x9, #0]
    stp     x21, x22, [x9, #16]
    stp     x23, x24, [x9, #32]
    stp     x25, x26, [x9, #48]
    stp     x27, x28, [x9, #64]
    stp     x29, x30, [x9, #80]
    stp     d8, d9,   [x9, #96]
    stp     d10, d11, [x9, #112]
    stp     d12, d13, [x9, #128]
    stp     d14, d15, [x9, #144]
    mov     x10, sp
    mrs     x11, tpidr_el0
    stp     x10, x11, [x9, #160]
    b       lx_island_start
    .size lx_island_main, .-lx_island_main

    .global lx_island_return
    .hidden lx_island_return
    .type lx_island_return, %function
lx_island_return:
    adrp    x9, lx_host_frame
    add     x9, x9, :lo12:lx_host_frame
    ldp     x10, x11, [x9, #160]
    mov     sp, x10
    msr     tpidr_el0, x11
    ldp     x19, x20, [x9, #0]
    ldp     x21, x22, [x9, #16]
    ldp     x23, x24, [x9, #32]
    ldp     x25, x26, [x9, #48]
    ldp     x27, x28, [x9, #64]
    ldp     x29, x30, [x9, #80]
    ldp     d8, d9,   [x9, #96]
    ldp     d10, d11, [x9, #112]
    ldp     d12, d13, [x9, #128]
    ldp     d14, d15, [x9, #144]
    ret
    .size lx_island_return, .-lx_island_return

/* lx_call_on_stack(fn, arg, sp): jump to fn(arg) on another stack; fn must not return. */
    .global lx_call_on_stack
    .hidden lx_call_on_stack
    .type lx_call_on_stack, %function
lx_call_on_stack:
    mov     x9, x0
    mov     x0, x1
    and     x2, x2, #~15
    mov     sp, x2
    mov     x29, #0
    mov     x30, #0
    br      x9
    .size lx_call_on_stack, .-lx_call_on_stack

    .bss
    .align 4
lx_host_frame:
    .space 176

    .section .note.GNU-stack, "", %progbits
