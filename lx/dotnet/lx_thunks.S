/*
 * NativeAOT delegate thunks for the island (Marshal.GetFunctionPointerForDelegate, delegates passed to native code:
 * Realm's callbacks, FFmpeg's avio callbacks, ...).
 *
 * The runtime builds its thunk block in mmap'd memory and mprotects it executable; heap memory can't execute here.
 * HBLauncher used a prebuilt block whose data sat exactly 0x8000 after the code (`adr x16, . + 0x8000`), which a
 * libounce .nro can't provide (read-only data lies between code and writable data). This block is the same layout
 * - 8 code pages of 255 16-byte thunks, last 16 bytes of each page unused - but every thunk reaches its data slot
 * with adrp/add, wherever lx_thunk_data is:
 *     adrp x16, <slot>
 *     add  x16, x16, :lo12:<slot>
 *     ldr  x17, [x16, #<this data page's common stub pointer, its last 8 bytes>]
 *     br   x17
 * The data block mirrors the code block page for page (thunk at code page p, offset o <-> data at data page p,
 * offset o). lx_dotnet.c hands the block out and wraps the runtime's code<->data address conversions to match.
 */
        .section .text.lx_thunks, "ax", %progbits
        .p2align 12
        .globl lx_thunks_start
        .hidden lx_thunks_start
lx_thunks_start:
        .set page, 0
        .rept 8
        .set slot, 0
        .rept 255
        adrp    x16, lx_thunk_data + page * 4096 + slot * 16
        add     x16, x16, :lo12:(lx_thunk_data + page * 4096 + slot * 16)
        ldr     x17, [x16, #(4096 - 8 - 16 * slot)]
        br      x17
        .set slot, slot + 1
        .endr
        .zero   16
        .set page, page + 1
        .endr

        .section .data.lx_thunk_data, "aw", %progbits
        .p2align 12
        .globl lx_thunk_data
        .hidden lx_thunk_data
lx_thunk_data:
        .zero   0x8000

        .section .note.GNU-stack, "", %progbits
