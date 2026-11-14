/*
 * lx_thread_enter(fn, arg, sp, tls): run an island thread's start routine fn(arg) with sp and tpidr_el0 as musl set
 * them up for the new thread (clone semantics). If fn returns, its value goes to lx_thread_fn_returned, which ends
 * the thread like SYS_exit. Never returns.
 */
    .text
    .global lx_thread_enter
    .type lx_thread_enter, %function
lx_thread_enter:
    msr     tpidr_el0, x3
    and     x2, x2, #~15
    mov     sp, x2
    mov     x29, #0
    mov     x30, #0
    mov     x9, x0
    mov     x0, x1
    blr     x9
    bl      lx_thread_fn_returned
    brk     #0
    .size lx_thread_enter, .-lx_thread_enter

    .section .note.GNU-stack, "", %progbits
