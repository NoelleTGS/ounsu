#!/bin/sh
# Build the networking probe as an HBLauncher Linux-personality app (TESTING.md test 7).
#   HBL_ROOT        HBLauncher checkout   (default ~/HBLauncher)
#   HBXL_TOOLCHAIN  patched musl toolchain (default ../../toolchain-src/toolchain)
#   HBX_FS_HOST     PC address for the file server (e.g. 192.168.1.206)
#   desktop         `sh build.sh desktop`: linux-x64 NativeAOT build in obj/desktop (same Program.cs, real Linux)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
HBL_ROOT=${HBL_ROOT:-$HOME/HBLauncher}
cd "$HERE"
if [ "$1" = desktop ]; then
  dotnet publish -c Release -nologo -r linux-x64 -p:StaticExecutable=false -o obj/desktop
  exit 0
fi
export HBXL_TOOLCHAIN=${HBXL_TOOLCHAIN:-$ROOT/toolchain-src/toolchain}
export HBXL_CAPTURE="$HERE/obj/link-capture.txt"
TOOLS="$ROOT/probes/aot-hello/tools"
SSL="$ROOT/src/native/openssl"
SHIM="$ROOT/console/osu-game/shim"
mkdir -p obj
python3 "$ROOT/probes/gfx-probe/tools/gen-ssl-syms.py" "$HBXL_TOOLCHAIN/bin/aarch64-linux-musl-nm" \
  obj/ssl_syms.c "$SSL/libssl.a" "$SSL/libcrypto.a"
cat > obj/app.base.json <<EOF
{
  "id": "net-probe",
  "name": "Net Probe",
  "description": "osu! port test: sockets, epoll, DNS, TLS, HttpClient, WebSocket",
  "sources": ["../../../console/osu-game/shim/hbxl_thread_stacks.c",
              "../../../console/osu-game/shim/hbxl_net.c", "../shim/hbxl_dl_ssl.c", "../obj/ssl_syms.c",
              "../../gfx-probe/shim/hbxl_thunks.S", "../../gfx-probe/shim/hbxl_thunks.c"],
  "linux": {
    "code_base": "0x0c000000",
    "data_base": "after_code",
    "ldflags": ["-Wl,--wrap=pthread_create", "-Wl,--wrap=RhAllocateThunksMapping", "-Wl,--wrap=_Z9PalHijackP6Thread",
                "-Wl,--wrap=getaddrinfo", "-Wl,--wrap=freeaddrinfo",
                "-Wl,--start-group", "$SSL/libssl.a", "$SSL/libcrypto.a", "-Wl,--end-group"]
  }
}
EOF
dotnet publish -c Release -nologo -p:CppCompilerAndLinker="$TOOLS/capture-link.sh"
python3 "$TOOLS/make-manifest.py" "$HBXL_CAPTURE" "$HERE/app" "$HERE/obj/app.base.json"
# make-manifest's default source is aot-hello's dotnet shim; use osu-game's (GC activation emulation).
python3 - "$HERE/app/manifest.json" <<'EOF'
import json, sys
p = sys.argv[1]
m = json.load(open(p))
m["sources"] = ["../../../console/osu-game/shim/hbxl_dotnet_shim.c" if s == "../shim/hbxl_dotnet_shim.c" else s
                for s in m["sources"]]
open(p, "w").write(json.dumps(m, indent=2) + "\n")
EOF
python3 "$HBL_ROOT/sdk/tools/hbxl-build.py" "$HERE/app"
