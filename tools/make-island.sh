#!/bin/sh
# Links a program built with the aarch64-linux-musl toolchain into one island object for the lx host.
#
#   sh tools/make-island.sh OUT.o INPUT...
#
# INPUTs are the program's objects, archives and linker flags (they go on the musl gcc command line in order). The
# island runtime (lx/island) comes first, musl's libc.a and libgcc last. The result has exactly one global symbol,
# lx_island_main, and no undefined symbols: everything else is local, so the island's libc can't meet newlib in the
# final link.
#
# LX_MUSL_TOOLCHAIN: the musl toolchain (default: toolchain/ in this repository; GCC 15.1 + musl 1.2.6 with the
# syscall hooks, patches/musl.md).
set -eu

[ $# -ge 2 ] || { echo "usage: $0 OUT.o INPUT..." >&2; exit 2; }
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TC=${LX_MUSL_TOOLCHAIN:-$ROOT/toolchain}
CC="$TC/bin/aarch64-linux-musl-gcc"
OBJCOPY="$TC/bin/aarch64-linux-musl-objcopy"
NM="$TC/bin/aarch64-linux-musl-nm"
[ -x "$CC" ] || { echo "make-island: no musl toolchain at $TC" >&2; exit 1; }

OUT=$1
shift
WORK=$(mktemp -d)
[ -n "${LX_KEEP_WORK:-}" ] && echo "make-island: work directory $WORK" || trap 'rm -rf "$WORK"' EXIT

CFLAGS="-O2 -g -Wall -Wextra -Werror -fPIE -ffunction-sections -fdata-sections -I$ROOT/lx/include"
"$CC" $CFLAGS -c "$ROOT/lx/island/lx_island.c" -o "$WORK/lx_island.o"
"$CC" -g -c "$ROOT/lx/island/lx_island_asm.S" -o "$WORK/lx_island_asm.o"
"$CC" $CFLAGS -c "$ROOT/lx/island/lx_net.c" -o "$WORK/lx_net.o"

LIBC=$("$CC" -print-file-name=libc.a)
LIBGCC=$("$CC" -print-libgcc-file-name)
LIBGCC_EH=$("$CC" -print-file-name=libgcc_eh.a)

# --force-group-allocation dissolves COMDAT groups (C++ templates, typeinfo, inline functions): the final link
# deduplicates groups by name, and with C++ on the host side too (OunceVK) it kept the host's copy of a group and
# dropped the island's, leaving the island's references dangling (typeinfo for __cxxabiv1::__forced_unwind).
island_link() { # extra linker flags...
    "$CC" -nostdlib -nostartfiles -r -Wl,-T,"$ROOT/lx/island/island-r.ld" -o "$WORK/full.o" \
        -Wl,--force-group-allocation -Wl,--wrap=getaddrinfo -Wl,--wrap=freeaddrinfo "$@" \
        "$WORK/lx_island.o" "$WORK/lx_island_asm.o" "$WORK/lx_net.o" $INPUTS \
        -Wl,--start-group "$LIBC" "$LIBGCC" "$LIBGCC_EH" -Wl,--end-group
}
weak_undefined() {
    "$NM" -u "$WORK/full.o" | awk '$1 == "w" && $2 != "_DYNAMIC" {print $2}'
}

# Weak references: a weak reference doesn't pull an archive member in (OpenSSL's weak getentropy stayed undefined
# although musl has it), and one left undefined would bind to a host (newlib) symbol of that name in the final link.
# So: link again with -u for each, letting the archives supply what they have; whatever is still missing is pinned
# to 0 inside the island, which is what an unresolved weak reference means.
INPUTS=""
for a in "$@"; do INPUTS="$INPUTS $(printf '%s' "$a" | sed 's/ /\\ /g')"; done
island_link
WEAK=$(weak_undefined)
if [ -n "$WEAK" ]; then
    PULL=$(for w in $WEAK; do printf ' -Wl,-u,%s' "$w"; done)
    island_link $PULL
    WEAK=$(weak_undefined)
    if [ -n "$WEAK" ]; then
        echo "make-island: weak references left at 0: $(echo $WEAK)"
        island_link $PULL $(for w in $WEAK; do printf ' -Wl,--defsym,%s=0' "$w"; done)
    fi
fi

# Undefined symbols that no relocation uses are debris (the .NET runtime's WriteBarriers.S.o lists its macro
# parameters "dst" and "val" that way); drop them so they can't meet anything in the final link.
STRIP=""
UNUSED=$(python3 "$ROOT/tools/unused-undef.py" "$WORK/full.o")
for sym in $UNUSED; do
    STRIP="$STRIP --strip-symbol=$sym"
    echo "make-island: dropping unused undefined symbol $sym"
done
"$OBJCOPY" $STRIP --keep-global-symbol=lx_island_main "$WORK/full.o" "$OUT"

# Checks: nothing undefined, one global, and no `svc` left (a raw Linux syscall would be a Horizon SVC 0).
# Allowed: musl's weak _DYNAMIC (static TLS set-up uses it only together with a PT_DYNAMIC program header, and the
# island's synthetic headers have none; in the final PIE link it binds to the host's _DYNAMIC, unused).
UNDEF=$("$NM" -u "$OUT" | grep -v -E '^\s+w _DYNAMIC$' || true)
if [ -n "$UNDEF" ]; then
    echo "make-island: undefined symbols in the island:" >&2
    echo "$UNDEF" >&2
    exit 1
fi
GLOBALS=$("$NM" -g --defined-only "$OUT" | awk '{print $3}')
if [ "$GLOBALS" != "lx_island_main" ]; then
    echo "make-island: unexpected global symbols:" >&2
    echo "$GLOBALS" >&2
    exit 1
fi
# Allowed: musl's signal-return trampoline __restore(_rt) (rt_sigreturn). sigaction hands it to the "kernel" as the
# restorer, and the host's sigaction never delivers a signal, so it never runs.
if ! SVC=$(python3 "$ROOT/tools/svc-scan.py" "$OUT" __restore __restore_rt); then
    echo "make-island: raw svc instructions in the island:" >&2
    echo "$SVC" >&2
    exit 1
fi
echo "make-island: $OUT ($(du -k "$OUT" | cut -f1) KiB)"
