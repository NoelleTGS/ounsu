#!/bin/sh
# Builds the NativeAOT probe for kaiHEN: dotnet publish (ILC; the link step is captured, not run) -> island
# (tools/aot-island.py) -> .nro (lx host). Output: build/aot-hello.nro
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
export LX_CAPTURE="$HERE/obj/link-capture.txt"
mkdir -p "$HERE/obj" "$HERE/build"
cd "$HERE"
dotnet publish -c Release -nologo -p:CppCompilerAndLinker="$HERE/tools/capture-link.sh"
python3 "$ROOT/tools/aot-island.py" "$LX_CAPTURE" "$HERE" "$HERE/build/island.o"
make -s -C "$ROOT/lx/host" ISLAND="$HERE/build/island.o" APP=aot-hello APP_TITLE="lx AOT hello" OUT="$HERE/build"
ls -la "$HERE/build/aot-hello.nro"
