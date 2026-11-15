#!/bin/sh
# Builds the static musl libraries the island links (console/osu-game/build.sh uses them):
#
#   sh tools/build-native.sh [sdl] [sqlite] [openssl] [ffmpeg] [veldrid-spirv] [realm]   (no argument: all of them)
#   sh tools/build-native.sh ffmpeg-desktop   FFmpeg as x86_64 shared libraries for the desktop comparison build
#
# Each checkout goes in src/native/<name> at the commit in patches/<name>.base, with patches/<name>.diff applied (see
# patches/<name>.md); SDL and FFmpeg are unmodified. SQLite is the amalgamation in src/native/sqlite-dl (patches/sqlite.md).
# Output: src/native/<name>/build-lx (realm: src/native/realm-dotnet/wrappers/build-lx; OpenSSL builds in its tree).
#
# LX_MUSL_TOOLCHAIN: the aarch64-linux-musl toolchain (default: toolchain/ in this repository; patches/musl.md).
# Needs cmake, ninja, make and perl.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TC=${LX_MUSL_TOOLCHAIN:-$ROOT/toolchain}
export LX_MUSL_TOOLCHAIN="$TC"
NATIVE="$ROOT/src/native"
CROSS="$TC/bin/aarch64-linux-musl-"
FLAGS="-mbranch-protection=bti -fno-pie -fno-pic -ffunction-sections -fdata-sections"
JOBS=$(nproc)

die() { echo "build-native: $*" >&2; exit 1; }
[ -x "${CROSS}gcc" ] || die "no musl toolchain at $TC (set LX_MUSL_TOOLCHAIN)"
need() { [ -e "$1" ] || die "missing $1: check out the sources first (patches/$2.md)"; }

cmake_lib() { # source build-dir target ("" = all) cmake-options...
    src=$1 out=$2 target=$3
    shift 3
    cmake -G Ninja -S "$src" -B "$out" -DCMAKE_TOOLCHAIN_FILE="$ROOT/tools/musl-toolchain.cmake" \
        -DCMAKE_BUILD_TYPE=Release "$@" > "$out.cfg.log" 2>&1 || { tail -30 "$out.cfg.log"; die "configuring $src failed"; }
    cmake --build "$out" -j "$JOBS" ${target:+--target "$target"} > "$out.log" 2>&1 ||
        { tail -40 "$out.log"; die "building $src failed"; }
}

# SDL 3: only the offscreen video driver and the dummy audio driver are used (the console's display, audio and input go
# through the host); SDL_SRC overrides the source folder.
build_sdl() {
    src=${SDL_SRC:-$NATIVE/SDL}
    need "$src/CMakeLists.txt" SDL
    mkdir -p "$NATIVE/SDL"
    cmake_lib "$src" "$NATIVE/SDL/build-lx" "" -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TESTS=OFF -DSDL_TEST_LIBRARY=OFF \
        -DSDL_EXAMPLES=OFF -DSDL_UNIX_CONSOLE_BUILD=ON
    ls -la "$NATIVE/SDL/build-lx/libSDL3.a"
}

# SQLite with SQLitePCLRaw's e_sqlite3 options (osu! reads old lazer databases through it).
build_sqlite() {
    src="$NATIVE/sqlite-dl/sqlite-amalgamation-3500400"
    out="$NATIVE/sqlite-dl/build-lx"
    need "$src/sqlite3.c" sqlite
    mkdir -p "$out"
    "${CROSS}gcc" -O2 $FLAGS -DSQLITE_THREADSAFE=1 -DSQLITE_DEFAULT_MMAP_SIZE=0 -DSQLITE_DEFAULT_FOREIGN_KEYS=1 \
        -DSQLITE_ENABLE_COLUMN_METADATA -DSQLITE_ENABLE_FTS4 -DSQLITE_ENABLE_FTS5 -DSQLITE_ENABLE_RTREE \
        -DSQLITE_ENABLE_SNAPSHOT -DSQLITE_ENABLE_MATH_FUNCTIONS -c "$src/sqlite3.c" -o "$out/sqlite3.o"
    rm -f "$out/libe_sqlite3.a"
    "${CROSS}ar" rcs "$out/libe_sqlite3.a" "$out/sqlite3.o"
    ls -la "$out/libe_sqlite3.a"
}

# OpenSSL 3.5: libssl.a + libcrypto.a for .NET's crypto and TLS. OPENSSLDIR stays /usr/local/ssl (the CA bundle is in
# the .nro's romfs there).
build_openssl() {
    src="$NATIVE/openssl"
    need "$src/Configure" openssl
    (
        cd "$src"
        CC="${CROSS}gcc" AR="${CROSS}ar" RANLIB="${CROSS}ranlib" ./Configure linux-aarch64 no-shared no-dso no-async \
            no-tests no-docs no-apps no-engine no-afalgeng no-module no-legacy $FLAGS > configure-lx.log 2>&1 ||
            { tail -30 configure-lx.log; exit 1; }
        make -j"$JOBS" build_libs > make-lx.log 2>&1 || { tail -40 make-lx.log; exit 1; }
    ) || die "building OpenSSL failed"
    ls -la "$src/libssl.a" "$src/libcrypto.a"
}

# FFmpeg 7.1.1 (matches FFmpeg.AutoGen 7.1.1), audio only: the decoders, demuxers and resampler mysoundlib needs.
# Position-independent (--enable-pic): its NEON assembly otherwise has absolute addresses in code, which the .nro's
# PIE link refuses.
build_ffmpeg() {
    src="$NATIVE/ffmpeg"
    need "$src/configure" ffmpeg
    if [ "${1:-}" = desktop ]; then
        out="$src/build-desktop"
        set -- --enable-shared --disable-static
    else
        out="$src/build-lx"
        set -- --enable-cross-compile --arch=aarch64 --target-os=linux --cross-prefix="$CROSS" \
            --sysroot="$TC/aarch64-linux-musl" --extra-cflags="-fPIE -ffunction-sections -fdata-sections" \
            --enable-static --disable-shared --enable-pic
    fi
    mkdir -p "$out/obj"
    (
        cd "$out/obj"
        "$src/configure" "$@" --prefix="$out" --pkg-config=false \
            --disable-programs --disable-doc --disable-debug \
            --disable-avdevice --disable-avfilter --disable-swscale --disable-postproc \
            --disable-network --disable-autodetect --disable-iconv \
            --disable-everything \
            --enable-protocol=file,pipe \
            --enable-demuxer=mp3,ogg,wav,flac,aac,mov,aiff \
            --enable-parser=mpegaudio,vorbis,opus,aac,flac \
            --enable-decoder=mp3,mp3float,mp2,mp2float,vorbis,opus,aac,aac_fixed,flac,alac \
            --enable-decoder=pcm_s16le,pcm_s16be,pcm_u8,pcm_s24le,pcm_s32le,pcm_f32le,pcm_f64le,adpcm_ms,adpcm_ima_wav \
            > "$out/configure.log" 2>&1 || { tail -30 "$out/configure.log"; exit 1; }
        make -j"$JOBS" > "$out/make.log" 2>&1 || { tail -40 "$out/make.log"; exit 1; }
        make install > /dev/null
    ) || die "building FFmpeg failed"
    ls -la "$out/lib"
}

# veldrid-spirv (SPIRV-Cross + shaderc): osu-framework compiles its shaders at runtime through it. Only the library and
# what it needs (build.sh links every archive in build-lx, and shaderc_combined would duplicate glslang).
build_spirv() {
    need "$NATIVE/veldrid-spirv/CMakeLists.txt" veldrid-spirv
    cmake_lib "$NATIVE/veldrid-spirv" "$NATIVE/veldrid-spirv/build-lx" veldrid-spirv
    find "$NATIVE/veldrid-spirv/build-lx" -name 'libveldrid-spirv.a' -exec ls -la {} +
}

# Realm's native wrappers + realm-core. No encryption (no OpenSSL inside Realm), no epoll (the generic commit helper).
build_realm() {
    need "$NATIVE/realm-dotnet/wrappers/realm-core/CMakeLists.txt" realm-dotnet
    cmake_lib "$NATIVE/realm-dotnet/wrappers" "$NATIVE/realm-dotnet/wrappers/build-lx" "" -DREALM_ENABLE_ENCRYPTION=OFF \
        -DREALM_HAVE_EPOLL=OFF -DREALM_NO_TESTS=ON -DREALM_BUILD_LIB_ONLY=ON -DREALM_DOTNET_BUILD_CORE_FROM_SOURCE=ON
    find "$NATIVE/realm-dotnet/wrappers/build-lx" -name '*.a' -exec ls -la {} +
}

[ $# -gt 0 ] || set -- sdl sqlite openssl ffmpeg veldrid-spirv realm
for lib in "$@"; do
    echo "build-native: $lib"
    case "$lib" in
        sdl) build_sdl ;;
        sqlite) build_sqlite ;;
        openssl) build_openssl ;;
        ffmpeg) build_ffmpeg ;;
        ffmpeg-desktop) build_ffmpeg desktop ;;
        veldrid-spirv) build_spirv ;;
        realm) build_realm ;;
        *) die "unknown library $lib" ;;
    esac
done
