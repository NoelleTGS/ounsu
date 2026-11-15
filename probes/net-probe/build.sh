#!/bin/sh
# Builds the networking probe for kaiHEN (TESTING.md test 5): dotnet publish (link captured), island with the static
# OpenSSL and its symbol table, .nro whose romfs carries the probe server's CA and the CA bundle (so nothing has to
# be copied to the SD card). Run tools/net-probe-server.py once first: it creates the test CA.
#   sh build.sh desktop   linux-x64 NativeAOT build in obj/desktop (same Program.cs, real Linux)
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
TC=${LX_MUSL_TOOLCHAIN:-$ROOT/toolchain}
cd "$HERE"
if [ "${1:-}" = desktop ]; then
    dotnet publish -c Release -nologo -r linux-x64 -p:StaticExecutable=false -o obj/desktop
    exit 0
fi
SSL="$ROOT/src/native/openssl"
CA="$HERE/certs/ca.pem"
[ -f "$CA" ] || { echo "net-probe: no $CA; start tools/net-probe-server.py once first" >&2; exit 1; }
export LX_CAPTURE="$HERE/obj/link-capture.txt"
mkdir -p "$HERE/obj" "$HERE/build" "$HERE/obj/romfs/net-probe" "$HERE/obj/romfs/usr/local/ssl"
install -m 644 "$CA" "$HERE/obj/romfs/net-probe/ca.pem"
printf 'NETPROBE_HOST=%s\n' "$(cat "$HERE/certs/host.txt")" > "$HERE/obj/romfs/lx-env"   # the server's PC address
install -m 644 "$ROOT/fsroot/usr/local/ssl/cert.pem" "$HERE/obj/romfs/usr/local/ssl/cert.pem"
python3 "$ROOT/tools/gen-ssl-syms.py" "$TC/bin/aarch64-linux-musl-nm" "$HERE/obj/ssl_syms.c" "$SSL/libssl.a" "$SSL/libcrypto.a"
dotnet publish -c Release -nologo -p:CppCompilerAndLinker="$ROOT/tools/capture-link.sh"
python3 "$ROOT/tools/aot-island.py" "$LX_CAPTURE" "$HERE" "$HERE/build/island.o" "$HERE/obj/ssl_syms.c" \
    -Wl,--start-group "$SSL/libssl.a" "$SSL/libcrypto.a" -Wl,--end-group
make -s -C "$ROOT/lx/host" ISLAND="$HERE/build/island.o" APP=net-probe APP_TITLE="lx Net probe" OUT="$HERE/build" \
    ROMFS="$HERE/obj/romfs"
ls -la "$HERE/build/net-probe.nro"
