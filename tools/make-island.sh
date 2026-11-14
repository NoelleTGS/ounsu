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
# LX_MUSL_TOOLCHAIN: the musl toolchain (default ~/osu-HBLauncher/toolchain-src/toolchain: GCC 15.1 + musl 1.2.6 with
# the hbx syscall hooks).
set -eu

[ $# -ge 2 ] || { echo "usage: $0 OUT.o INPUT..." >&2; exit 2; }
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TC=${LX_MUSL_TOOLCHAIN:-$HOME/osu-HBLauncher/toolchain-src/toolchain}
CC="$TC/bin/aarch64-linux-musl-gcc"
OBJCOPY="$TC/bin/aarch64-linux-musl-objcopy"
NM="$TC/bin/aarch64-linux-musl-nm"
OBJDUMP="$TC/bin/aarch64-linux-musl-objdump"
[ -x "$CC" ] || { echo "make-island: no musl toolchain at $TC" >&2; exit 1; }

OUT=$1
shift
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

CFLAGS="-O2 -g -Wall -Wextra -Werror -fPIE -ffunction-sections -fdata-sections -I$ROOT/lx/include"
"$CC" $CFLAGS -c "$ROOT/lx/island/lx_island.c" -o "$WORK/lx_island.o"
"$CC" -g -c "$ROOT/lx/island/lx_island_asm.S" -o "$WORK/lx_island_asm.o"

LIBC=$("$CC" -print-file-name=libc.a)
LIBGCC=$("$CC" -print-libgcc-file-name)
LIBGCC_EH=$("$CC" -print-file-name=libgcc_eh.a)

"$CC" -nostdlib -nostartfiles -r -Wl,-T,"$ROOT/lx/island/island-r.ld" -o "$WORK/full.o" \
    "$WORK/lx_island.o" "$WORK/lx_island_asm.o" "$@" \
    -Wl,--start-group "$LIBC" "$LIBGCC" "$LIBGCC_EH" -Wl,--end-group

"$OBJCOPY" --keep-global-symbol=lx_island_main "$WORK/full.o" "$OUT"

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
SVC=$("$OBJDUMP" -d "$OUT" | grep -B30 -E '\ssvc\s' | grep -E '^[0-9a-f]+ <|\ssvc\s' || true)
if [ -n "$SVC" ]; then
    echo "make-island: raw svc instructions in the island (functions shown):" >&2
    echo "$SVC" >&2
    exit 1
fi
echo "make-island: $OUT ($(du -k "$OUT" | cut -f1) KiB)"
