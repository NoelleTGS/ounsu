#!/bin/sh
# Build the graphics probe as an exclusive-display HBLauncher Linux-personality app.
#   HBL_ROOT        HBLauncher checkout   (default ~/HBLauncher)
#   HBXL_TOOLCHAIN  patched musl toolchain (default ../../toolchain-src/toolchain)
#   HBX_FS_HOST     PC address for the file server (e.g. 192.168.1.206)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
HBL_ROOT=${HBL_ROOT:-$HOME/HBLauncher}
export HBXL_TOOLCHAIN=${HBXL_TOOLCHAIN:-$HERE/../../toolchain-src/toolchain}
export HBXL_CAPTURE="$HERE/obj/link-capture.txt"
TOOLS="$HERE/../aot-hello/tools"
mkdir -p "$HERE/obj/shaders" "$HERE/obj/vk"
cd "$HERE"
glslc -O shaders/tri.vert -o obj/shaders/tri.vert.spv
glslc -O shaders/tri.frag -o obj/shaders/tri.frag.spv
glslc -O shaders/fs.vert -o obj/shaders/fs.vert.spv
glslc -O shaders/solid.frag -o obj/shaders/solid.frag.spv
glslc -O shaders/tex.frag -o obj/shaders/tex.frag.spv
glslc -O shaders/idx.vert -o obj/shaders/idx.vert.spv
glslc -O shaders/ubo.frag -o obj/shaders/ubo.frag.spv
glslc -O shaders/ubotex.frag -o obj/shaders/ubotex.frag.spv
python3 tools/gen-vk-tramp.py "$HERE/../../vk-include/vulkan/vulkan_core.h" obj/vk
dotnet publish -c Release -nologo -p:CppCompilerAndLinker="$TOOLS/capture-link.sh"
python3 "$TOOLS/make-manifest.py" "$HBXL_CAPTURE" "$HERE/app" "$HERE/app.base.json"
python3 "$HBL_ROOT/sdk/tools/hbxl-build.py" "$HERE/app"
