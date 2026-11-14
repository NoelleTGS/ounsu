#!/bin/sh
# Build the NativeAOT probe as an HBLauncher Linux-personality app.
#   HBL_ROOT        HBLauncher checkout   (default ~/HBLauncher)
#   HBXL_TOOLCHAIN  patched musl toolchain (default ../../toolchain-src/toolchain)
# Output: app/ (manifest.json + build/code.bin, data.bin, report.json)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
HBL_ROOT=${HBL_ROOT:-$HOME/HBLauncher}
export HBXL_TOOLCHAIN=${HBXL_TOOLCHAIN:-$HERE/../../toolchain-src/toolchain}
export HBXL_CAPTURE="$HERE/obj/link-capture.txt"
mkdir -p "$HERE/obj"
cd "$HERE"
dotnet publish -c Release -nologo -p:CppCompilerAndLinker="$HERE/tools/capture-link.sh"
python3 tools/make-manifest.py "$HBXL_CAPTURE" "$HERE/app"
python3 "$HBL_ROOT/sdk/tools/hbxl-build.py" "$HERE/app"
