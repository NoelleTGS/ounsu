#!/bin/sh
# Builds probes/hello-island into build/hello-island.nro (musl island + lx host).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
TC=${LX_MUSL_TOOLCHAIN:-$ROOT/toolchain}
OUT="$HERE/build"
mkdir -p "$OUT"

"$TC/bin/aarch64-linux-musl-gcc" -O2 -g -Wall -Wextra -Werror -fPIE -ffunction-sections -fdata-sections \
    -c "$HERE/hello.c" -o "$OUT/hello.o"
sh "$ROOT/tools/make-island.sh" "$OUT/island.o" "$OUT/hello.o"
make -s -C "$ROOT/lx/host" ISLAND="$OUT/island.o" APP=hello-island APP_TITLE="lx hello" OUT="$OUT"
ls -la "$OUT/hello-island.nro"
