#!/bin/sh
# Builds ounsu! (osu! lazer for kaiHEN): build/ounsu.nro.
#
#   sh console/osu-game/build.sh                     full build (dotnet publish + NativeAOT compile, ~15 minutes)
#   LX_SKIP_PUBLISH=1 sh console/osu-game/build.sh   relink only, after changing C code (~1 minute)
#
#   1. Symbol tables for the island's fake libdl: OpenSSL (.NET crypto, HTTPS) and FFmpeg (audio decoding).
#   2. (full build) rd.xml regenerated from the previous build's IL (tools/gen-rdxml); the committed copy covers a first
#      build. Then dotnet publish with -p:OunsuBuild=true (a global property: the framework's patches need it), the
#      link step captured instead of run (tools/capture-link.sh).
#   3. The island (tools/aot-island.py --vulkan): osu!, the NativeAOT runtime, Veldrid's Vulkan loader, SDL3, Realm,
#      SQLite, veldrid-spirv, OpenSSL, FFmpeg, libstdc++ and musl.
#   4. The .nro (lx host with OunceVK), whose romfs carries the CA bundle and the default environment (lx-env).
#
# Needs the musl toolchain (toolchain/ or LX_MUSL_TOOLCHAIN; patches/musl.md) and the native libraries
# (sh tools/build-native.sh). Optional: OUNSU_VERSION (see below), LX_ENV_EXTRA (extra lx-env lines for a test build,
# e.g. LX_ENV_EXTRA="LX_TRACE=1").
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
export LX_MUSL_TOOLCHAIN="${LX_MUSL_TOOLCHAIN:-$ROOT/toolchain}"
NM="$LX_MUSL_TOOLCHAIN/bin/aarch64-linux-musl-nm"
NATIVE="$ROOT/src/native"
export LX_CAPTURE="$HERE/obj/link-capture.txt"
mkdir -p "$HERE/obj/romfs/usr/local/ssl" "$HERE/build"
cd "$HERE"

[ -x "$NM" ] || { echo "build.sh: no musl toolchain at $LX_MUSL_TOOLCHAIN (patches/musl.md)" >&2; exit 1; }
SSL="$NATIVE/openssl"
FF="$NATIVE/ffmpeg/build-lx/lib"
SDL="$NATIVE/SDL/build-lx/libSDL3.a"
SQLITE="$NATIVE/sqlite-dl/build-lx/libe_sqlite3.a"
SPIRV_LIBS=$(find "$NATIVE/veldrid-spirv/build-lx" -name '*.a' 2>/dev/null | sort)
REALM_LIBS=$(find "$NATIVE/realm-dotnet/wrappers/build-lx" -name '*.a' 2>/dev/null | sort)
for f in "$SSL/libssl.a" "$FF/libavcodec.a" "$SDL" "$SQLITE"; do
    [ -f "$f" ] || { echo "build.sh: missing $f (sh tools/build-native.sh)" >&2; exit 1; }
done
[ -n "$SPIRV_LIBS" ] && [ -n "$REALM_LIBS" ] || { echo "build.sh: veldrid-spirv or Realm not built (sh tools/build-native.sh)" >&2; exit 1; }

python3 "$ROOT/tools/gen-ssl-syms.py" "$NM" obj/ssl_syms.c "$SSL/libssl.a" "$SSL/libcrypto.a"
python3 "$ROOT/tools/gen-ssl-syms.py" --table ffmpeg "$NM" obj/ffmpeg_syms.c \
    "$FF/libavformat.a" "$FF/libavcodec.a" "$FF/libswresample.a" "$FF/libavutil.a"

if [ "${LX_SKIP_PUBLISH:-0}" != 1 ]; then
    IL="$HERE/bin/Release/net10.0/linux-musl-arm64"
    if [ -f "$IL/osu.Game.dll" ]; then
        dotnet run --project "$ROOT/tools/gen-rdxml" -c Release -- "$IL" "$HERE/rd.xml"
    fi
    dotnet publish -c Release -nologo -p:OunsuBuild=true -p:CppCompilerAndLinker="$ROOT/tools/capture-link.sh"
fi

# veldrid-spirv is real C++ and links the real libstdc++: NativeAOT's stand-in has to go.
python3 "$ROOT/tools/aot-island.py" "$LX_CAPTURE" "$HERE" "$HERE/build/island.o" --vulkan --exclude libstdc++compat.a \
    -I"$NATIVE/SDL/include" -Wl,--wrap=SDL_PumpEvents -Wl,--wrap=SDL_OpenAudioDeviceStream \
    -Wl,--wrap=SDL_ResumeAudioStreamDevice -Wl,--wrap=SDL_PauseAudioStreamDevice -Wl,--wrap=SDL_DestroyAudioStream \
    -Wl,--wrap=RhpCollect -Wl,--wrap=_ZN11ThreadStore17SuspendAllThreadsEb \
    -Wl,--wrap=_ZN11ThreadStore16ResumeAllThreadsEb \
    "$HERE/shim/lx_osu.c" "$HERE/shim/lx_input.c" "$HERE/shim/lx_audio_out.c" "$HERE/shim/lx_gc.c" \
    "$HERE/shim/bass_stubs.S" "$HERE/shim/bass_devices.c" "$HERE/obj/ssl_syms.c" "$HERE/obj/ffmpeg_syms.c" "$SDL" \
    -Wl,--start-group $SPIRV_LIBS "$SSL/libssl.a" "$SSL/libcrypto.a" "$SQLITE" $REALM_LIBS \
    "$FF/libavformat.a" "$FF/libavcodec.a" "$FF/libswresample.a" "$FF/libavutil.a" -Wl,--end-group -lstdc++ -lm

# Absolute build paths (realm-core's assert strings, FFmpeg's and OpenSSL's configure lines, .NET attributes) would
# name the builder's home folder: overwrite them in place with a neutral path of the same length.
python3 "$ROOT/tools/scrub-paths.py" "$HERE/build/island.o" "$HOME"

install -m 644 "$ROOT/fsroot/usr/local/ssl/cert.pem" obj/romfs/usr/local/ssl/cert.pem
cat > obj/romfs/lx-env <<'EOF'
# ounsu!'s default environment (build.sh). Lines in /switch/ounsu/lx-env on the SD card come first and win.
SDL_VIDEO_DRIVER=offscreen
SDL_AUDIO_DRIVER=dummy
OSU_RESOURCES_DIR=/osu-resources
EOF
if [ -n "${LX_ENV_EXTRA:-}" ]; then printf '%s\n' $LX_ENV_EXTRA >> obj/romfs/lx-env; fi

# The .nro's hbmenu entry. osu-game's assembly version stays 0.0.0 (osu-game.csproj); the .nro's version uses osu!'s
# scheme, yyyy.Mdd.N from the build date, N being the first number not yet tagged in git. OUNSU_VERSION overrides it
# (e.g. OUNSU_VERSION=2026.1114.0 to finish a release after midnight). tools/release.sh reads build/ounsu.release.
if [ -n "${OUNSU_VERSION:-}" ]; then
    NRO_VERSION=$OUNSU_VERSION
else
    set -- $(date '+%Y %m %d')
    DAY="$1.$(echo "$2" | sed 's/^0//')$3"
    N=0
    while git -C "$ROOT" rev-parse -q --verify "refs/tags/$DAY.$N" >/dev/null; do N=$((N + 1)); done
    NRO_VERSION="$DAY.$N"
fi
DIRTY=0
if [ -n "$(git -C "$ROOT" status --porcelain --untracked-files=no)" ]; then DIRTY=1; fi
printf 'version=%s\ncommit=%s\ndirty=%s\n' "$NRO_VERSION" "$(git -C "$ROOT" rev-parse HEAD)" "$DIRTY" > "$HERE/build/ounsu.release"
make -s -C "$ROOT/lx/host" ISLAND="$HERE/build/island.o" APP=ounsu APP_TITLE="ounsu!" APP_AUTHOR="NoelleTGS" \
    APP_VERSION="$NRO_VERSION" ICON="$HERE/icon.jpg" OUT="$HERE/build" VULKAN=1 ROMFS="$HERE/obj/romfs"
ls -la "$HERE/build/ounsu.nro"
echo "build.sh: ounsu! $NRO_VERSION"
