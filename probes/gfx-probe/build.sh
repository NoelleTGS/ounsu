#!/bin/sh
# Builds the graphics probe for kaiHEN: SPIR-V shaders, dotnet publish (link captured), island with Veldrid's Vulkan
# loader (--vulkan), .nro with OunceVK (VULKAN=1). Output: build/gfx-probe.nro
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
export LX_CAPTURE="$HERE/obj/link-capture.txt"
mkdir -p "$HERE/obj/shaders" "$HERE/build"
cd "$HERE"
for s in tri.vert tri.frag fs.vert solid.frag tex.frag idx.vert ubo.frag ubotex.frag; do
    glslc -O "shaders/$s" -o "obj/shaders/$s.spv"
done
dotnet publish -c Release -nologo -p:CppCompilerAndLinker="$ROOT/tools/capture-link.sh"
python3 "$ROOT/tools/aot-island.py" "$LX_CAPTURE" "$HERE" "$HERE/build/island.o" --vulkan
make -s -C "$ROOT/lx/host" ISLAND="$HERE/build/island.o" APP=gfx-probe APP_TITLE="lx GFX probe" OUT="$HERE/build" VULKAN=1
ls -la "$HERE/build/gfx-probe.nro"
