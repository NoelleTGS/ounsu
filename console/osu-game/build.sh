#!/bin/sh
# Build osu! (cut-down) as an exclusive-display HBLauncher Linux-personality app.
#   HBL_ROOT        HBLauncher checkout   (default ~/HBLauncher)
#   HBXL_TOOLCHAIN  patched musl toolchain (default ../../toolchain-src/toolchain)
#   HBX_FS_HOST     PC address for the file server (e.g. 192.168.1.206)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
HBL_ROOT=${HBL_ROOT:-$HOME/HBLauncher}
export HBXL_TOOLCHAIN=${HBXL_TOOLCHAIN:-$ROOT/toolchain-src/toolchain}
export HBXL_CAPTURE="$HERE/obj/link-capture.txt"
TOOLS="$ROOT/probes/aot-hello/tools"
mkdir -p "$HERE/obj/vk"
cd "$HERE"
python3 "$ROOT/probes/gfx-probe/tools/gen-vk-tramp.py" "$ROOT/vk-include/vulkan/vulkan_core.h" obj/vk
SSL="$ROOT/src/native/openssl"
if [ -f "$SSL/libssl.a" ] && [ -f "$SSL/libcrypto.a" ]; then
  python3 "$ROOT/probes/gfx-probe/tools/gen-ssl-syms.py" "$HBXL_TOOLCHAIN/bin/aarch64-linux-musl-nm" \
    obj/ssl_syms.c "$SSL/libssl.a" "$SSL/libcrypto.a"
fi
FF="$ROOT/src/native/ffmpeg/build-hbx/lib"   # tools/build-ffmpeg-hbx.sh (audio decoding, mysoundlib)
if [ -f "$FF/libavcodec.a" ]; then
  python3 "$ROOT/probes/gfx-probe/tools/gen-ssl-syms.py" --table ffmpeg "$HBXL_TOOLCHAIN/bin/aarch64-linux-musl-nm" \
    obj/ffmpeg_syms.c "$FF/libavformat.a" "$FF/libavcodec.a" "$FF/libswresample.a" "$FF/libavutil.a"
fi
python3 "$HERE/tools/make-base.py" "$ROOT" > obj/app.base.json
# Launcher icon from osu!'s own logo (extracted resources; nothing is shipped). Optional.
if [ -f "$ROOT/fsroot/osu-resources/osu.Game.Resources.Textures.Menu.logo.png" ]; then
  python3 "$HERE/tools/make-icon.py" "$ROOT/fsroot/osu-resources" "$HERE/app/icon.png"
fi
# rd.xml: regenerate from the previous build's IL (tools/gen-rdxml); the committed copy covers a first build.
IL="$HERE/bin/Release/net10.0/linux-musl-arm64"
if [ -f "$IL/osu.Game.dll" ]; then
  dotnet run --project "$ROOT/tools/gen-rdxml" -c Release -- "$IL" "$HERE/rd.xml"
fi
dotnet publish -c Release -nologo -p:HbxlBuild=true -p:CppCompilerAndLinker="$TOOLS/capture-link.sh"
python3 "$TOOLS/make-manifest.py" "$HBXL_CAPTURE" "$HERE/app" "$HERE/obj/app.base.json"
python3 "$HBL_ROOT/sdk/tools/hbxl-build.py" "$HERE/app"
