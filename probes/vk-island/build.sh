#!/bin/sh
# Builds probes/vk-island into build/vk-island.nro (musl island + lx host with Vulkan).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
TC=${LX_MUSL_TOOLCHAIN:-$ROOT/toolchain}
OUT="$HERE/build"
mkdir -p "$OUT"

"$TC/bin/aarch64-linux-musl-gcc" -O2 -g -Wall -Wextra -Werror -fPIE -ffunction-sections -fdata-sections \
    -I"${DEVKITPRO:-/opt/devkitpro}/portlibs/switch2/include" -c "$HERE/vkclear.c" -o "$OUT/vkclear.o"
sh "$ROOT/tools/make-island.sh" "$OUT/island.o" "$OUT/vkclear.o" -lm
make -s -C "$ROOT/lx/host" ISLAND="$OUT/island.o" APP=vk-island APP_TITLE="lx Vulkan" OUT="$OUT" VULKAN=1
ls -la "$OUT/vk-island.nro"
