#!/bin/sh
# Builds ounsu! (osu! lazer, cut-down console build) for kaiHEN: build/ounsu.nro.
#
#   1. Symbol tables for the island's fake libdl: OpenSSL (.NET crypto, HTTPS) and FFmpeg (audio decoding).
#   2. (skipped with LX_SKIP_PUBLISH=1, which only relinks) rd.xml regenerated from the previous build's IL (tools/gen-rdxml); the committed copy covers a first build.
#   3. dotnet publish with -p:HbxlBuild=true (a global property: the framework's patches need it) and the link step
#      captured instead of run.
#   4. The island (tools/aot-island.py --vulkan): osu!, the NativeAOT runtime, Veldrid's Vulkan loader, SDL3, Realm,
#      SQLite, veldrid-spirv, OpenSSL, FFmpeg, libstdc++ and musl.
#   5. The .nro (lx host with OunceVK), whose romfs carries the CA bundle and the default environment (lx-env).
#
# LX_NATIVE_BUILDS: where the static native libraries built for HBLauncher are (default
# ~/osu-HBLauncher/src/native; they are musl builds and are reused unchanged). OpenSSL and FFmpeg (tools/build-ffmpeg.sh)
# are in this repo's src/native.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
TC=${LX_MUSL_TOOLCHAIN:-$HOME/osu-HBLauncher/toolchain-src/toolchain}
NATIVE=${LX_NATIVE_BUILDS:-$HOME/osu-HBLauncher/src/native}
NM="$TC/bin/aarch64-linux-musl-nm"
export LX_CAPTURE="$HERE/obj/link-capture.txt"
mkdir -p "$HERE/obj/romfs/usr/local/ssl" "$HERE/build"
cd "$HERE"

SSL="$ROOT/src/native/openssl"
FF="$ROOT/src/native/ffmpeg/build-lx/lib"   # tools/build-ffmpeg.sh (position-independent, unlike the hbxl build)
SPIRV_LIBS=$(find "$NATIVE/veldrid-spirv/build-hbx" -name '*.a' | sort)
REALM_LIBS=$(find "$NATIVE/realm-dotnet/wrappers/build-hbx" -name '*.a' | sort)
SQLITE="$NATIVE/sqlite-dl/build-hbx/libe_sqlite3.a"
SDL="$NATIVE/SDL/build-hbx/libSDL3.a"
for f in "$SSL/libssl.a" "$FF/libavcodec.a" "$SQLITE" "$SDL"; do
    [ -f "$f" ] || { echo "osu-game: missing $f" >&2; exit 1; }
done

python3 "$ROOT/tools/gen-ssl-syms.py" "$NM" obj/ssl_syms.c "$SSL/libssl.a" "$SSL/libcrypto.a"
python3 "$ROOT/tools/gen-ssl-syms.py" --table ffmpeg "$NM" obj/ffmpeg_syms.c \
    "$FF/libavformat.a" "$FF/libavcodec.a" "$FF/libswresample.a" "$FF/libavutil.a"

# LX_SKIP_PUBLISH=1: relink only (reuse the last ILC output and captured link line).
if [ "${LX_SKIP_PUBLISH:-0}" != 1 ]; then
    IL="$HERE/bin/Release/net10.0/linux-musl-arm64"
    if [ -f "$IL/osu.Game.dll" ]; then
        dotnet run --project "$ROOT/tools/gen-rdxml" -c Release -- "$IL" "$HERE/rd.xml"
    fi
    dotnet publish -c Release -nologo -p:HbxlBuild=true -p:CppCompilerAndLinker="$ROOT/probes/aot-hello/tools/capture-link.sh"
fi

# veldrid-spirv is real C++ and links the real libstdc++: NativeAOT's stand-in has to go.
python3 "$ROOT/tools/aot-island.py" "$LX_CAPTURE" "$HERE" "$HERE/build/island.o" --vulkan --exclude libstdc++compat.a \
    -I"$ROOT/src/native/SDL/include" -Wl,--wrap=SDL_PumpEvents -Wl,--wrap=SDL_OpenAudioDeviceStream \
    -Wl,--wrap=SDL_ResumeAudioStreamDevice -Wl,--wrap=SDL_PauseAudioStreamDevice -Wl,--wrap=SDL_DestroyAudioStream \
    -Wl,--wrap=RhpCollect -Wl,--wrap=_ZN11ThreadStore17SuspendAllThreadsEb \
    -Wl,--wrap=_ZN11ThreadStore16ResumeAllThreadsEb \
    "$HERE/shim/lx_osu.c" "$HERE/shim/lx_input.c" "$HERE/shim/lx_audio_out.c" "$HERE/shim/lx_gc.c" "$HERE/shim/bass_stubs.S" "$HERE/shim/bass_devices.c" \
    "$HERE/obj/ssl_syms.c" "$HERE/obj/ffmpeg_syms.c" "$SDL" \
    -Wl,--start-group $SPIRV_LIBS "$SSL/libssl.a" "$SSL/libcrypto.a" "$SQLITE" $REALM_LIBS \
    "$FF/libavformat.a" "$FF/libavcodec.a" "$FF/libswresample.a" "$FF/libavutil.a" -Wl,--end-group -lstdc++ -lm

# Absolute build paths (realm-core's assert strings, FFmpeg's and OpenSSL's configure lines, .NET attributes) would
# name the builder's home folder: overwrite them in place with a neutral path of the same length.
python3 "$ROOT/tools/scrub-paths.py" "$HERE/build/island.o" "$HOME"

install -m 644 "$ROOT/fsroot/usr/local/ssl/cert.pem" obj/romfs/usr/local/ssl/cert.pem
cat > obj/romfs/lx-env <<'EOF'
# osu-game's default environment (build.sh). Lines in /switch/ounsu/lx-env on the SD card come first and win.
SDL_VIDEO_DRIVER=offscreen
SDL_AUDIO_DRIVER=dummy
OSU_RESOURCES_DIR=/osu-resources
EOF
# LX_ENV_EXTRA: extra lines for a test build (e.g. LX_ENV_EXTRA="LX_TRACE=1").
if [ -n "${LX_ENV_EXTRA:-}" ]; then printf '%s\n' $LX_ENV_EXTRA >> obj/romfs/lx-env; fi
# The .nro's hbmenu entry (osu-game's assembly version stays 0.0.0, see CLAUDE.md). The version uses osu!'s scheme,
# yyyy.Mdd.N from the build date, N being the first number not yet tagged in git. OUNSU_VERSION overrides it (e.g.
# OUNSU_VERSION=2026.1114.0 to finish a release after midnight). tools/release.sh reads build/ounsu.release.
NRO_AUTHOR="NoelleTGS"
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
ICON_ARGS=""
if [ -f "$HERE/icon.jpg" ]; then ICON_ARGS="ICON=$HERE/icon.jpg"; fi
make -s -C "$ROOT/lx/host" ISLAND="$HERE/build/island.o" APP=ounsu APP_TITLE="ounsu!" APP_AUTHOR="$NRO_AUTHOR" \
    APP_VERSION="$NRO_VERSION" $ICON_ARGS OUT="$HERE/build" VULKAN=1 ROMFS="$HERE/obj/romfs"
ls -la "$HERE/build/ounsu.nro"
echo "build.sh: ounsu! $NRO_VERSION"
