/*
 * Linker script for the island's relocatable link (ld -r, musl toolchain).
 *
 * The host's crt0 runs the final image's .init_array/.fini_array (newlib's constructors). The island's musl must run
 * only the island's own, so they are moved into sections of their own here, with musl's boundary symbols pointing at
 * them. Every other section is kept as it is and placed by the final (devkitA64) link.
 */
SECTIONS
{
    /* Read-only data goes where the final link puts .data.rel.ro: the writable segment. musl and every library built
       by tools/build-native.sh are position-dependent code, so their constant tables hold absolute pointers (lock pointers,
       stderr, codec and name tables); in the position-independent .nro those need load-time fixups, which libounce's
       link (-z text) forbids in read-only memory. Exception tables too (non-PIC C++ may put absolute type pointers
       there). The cost: the island's constants are not write-protected. */
    .data.rel.ro.lx : {
        *(.rodata .rodata.* .gnu.linkonce.r.*)
        *(.data.rel.ro .data.rel.ro.*)
        *(.gcc_except_table .gcc_except_table.*)
    }

    .lx_preinit_array : {
        __preinit_array_start = .;
        KEEP (*(.preinit_array))
        __preinit_array_end = .;
    }
    .lx_init_array : {
        __init_array_start = .;
        KEEP (*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP (*(.init_array .ctors))
        __init_array_end = .;
    }
    .lx_fini_array : {
        __fini_array_start = .;
        KEEP (*(SORT_BY_INIT_PRIORITY(.fini_array.*) SORT_BY_INIT_PRIORITY(.dtors.*)))
        KEEP (*(.fini_array .dtors))
        __fini_array_end = .;
    }

    /* .NET NativeAOT: the bootstrapper finds the managed code, the module list and the unboxing stubs through
       __start_/__stop_ symbols, which ld only makes in a final link. Define them here so the island has no
       undefined symbols; the final link places the sections like any other (code with code, data with data). */
    __managedcode : {
        __start___managedcode = .;
        KEEP (*(__managedcode))
        __stop___managedcode = .;
    }
    __modules : {
        __start___modules = .;
        KEEP (*(__modules))
        __stop___modules = .;
    }
    __unbox : {
        __start___unbox = .;
        KEEP (*(__unbox))
        __stop___unbox = .;
    }
}
