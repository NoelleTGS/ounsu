/*
 * NativeAOT delegate thunks for hbxl (Marshal.GetFunctionPointerForDelegate, delegates passed
 * to native code: Realm's callbacks, FFmpeg avio callbacks, ...).
 *
 * The runtime's RhAllocateThunksMapping builds a 32 KiB block of thunk code in mmap'd memory
 * and mprotects it RX.  hbxl's mprotect is a no-op and mapped memory never executes, so the
 * first native call through a marshalled delegate killed the console silently.  This file
 * holds the same block prebuilt, byte for byte what the runtime generates (4 KiB pages, 255
 * 16-byte thunks per page, last 16 bytes unused):
 *     adr  x16, <own data slot, +0x8000>
 *     ldr  x17, [x16, #<page's common stub pointer, last 8 bytes of the data page>]
 *     br   x17
 *     brk  #0xf000
 * It goes last in the code image (.hbxl_thunks) and its data block first in the data region
 * (.hbxl_thunk_data), which hbxl.ld places exactly 0x8000 higher when HBXL_THUNKS is defined.
 * hbxl_thunks.c hands it out through -Wl,--wrap=RhAllocateThunksMapping.
 */
        .section .hbxl_thunks, "ax", %progbits
        .p2align 12
        .globl __hbxl_thunks_start
__hbxl_thunks_start:
        .rept 8
        .set slot, 0
        .rept 255
        adr     x16, . + 0x8000
        ldr     x17, [x16, #(4096 - 8 - 16 * slot)]
        br      x17
        brk     #0xf000
        .set slot, slot + 1
        .endr
        .zero   16
        .endr

        .section .hbxl_thunk_data, "aw", %progbits
        .p2align 12
        .globl __hbxl_thunk_data_start
__hbxl_thunk_data_start:
        .zero   0x8000

        .section .note.GNU-stack, "", %progbits
