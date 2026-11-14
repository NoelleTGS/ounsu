/*
 * Linker script for the island's relocatable link (ld -r, musl toolchain).
 *
 * The host's crt0 runs the final image's .init_array/.fini_array (newlib's constructors). The island's musl must run
 * only the island's own, so they are moved into sections of their own here, with musl's boundary symbols pointing at
 * them. Every other section is kept as it is and placed by the final (devkitA64) link.
 */
SECTIONS
{
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
}
