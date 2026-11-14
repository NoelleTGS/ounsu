#!/bin/sh
# Static FFmpeg 7.1.1 (matches FFmpeg.AutoGen 7.1.1) for the lx island, audio only.
# Source: git clone --depth 1 --branch n7.1.1 https://github.com/FFmpeg/FFmpeg.git src/native/ffmpeg
# Output: src/native/ffmpeg/build-lx/lib{avformat,avcodec,swresample,avutil}.a (+ include/)
# The island's fake libdl answers dlopen("libavcodec.so.61" ...) from obj/ffmpeg_syms.c (tools/gen-ssl-syms.py), so
# FFmpeg.AutoGen's DynamicallyLoadedBindings work unchanged.
# Position-independent (--enable-pic): the .nro is a PIE, and FFmpeg's NEON assembly built without PIC has absolute
# addresses in its code, which libounce's link refuses (text relocations). The HBLauncher build was --disable-pic.
#   build-ffmpeg.sh desktop  → the same configuration as shared x86_64 libs in build-desktop/lib
#   (libavcodec.so.61 ...) for the desktop comparison build: LD_LIBRARY_PATH=src/native/ffmpeg/build-desktop/lib
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TC=${LX_MUSL_TOOLCHAIN:-$HOME/osu-HBLauncher/toolchain-src/toolchain}
SRC="$ROOT/src/native/ffmpeg"
if [ "${1:-}" = desktop ]; then
  OUT="$SRC/build-desktop"
  set -- --enable-shared --disable-static
else
  OUT="$SRC/build-lx"
  set -- --enable-cross-compile --arch=aarch64 --target-os=linux \
    --cross-prefix="$TC/bin/aarch64-linux-musl-" \
    --sysroot="$TC/aarch64-linux-musl" \
    --extra-cflags="-fPIE -ffunction-sections -fdata-sections" \
    --enable-static --disable-shared --enable-pic
fi
mkdir -p "$OUT/obj"
cd "$OUT/obj"
"$SRC/configure" "$@" \
  --prefix="$OUT" \
  --pkg-config=false \
  --disable-programs --disable-doc --disable-debug \
  --disable-avdevice --disable-avfilter --disable-swscale --disable-postproc \
  --disable-network --disable-autodetect --disable-iconv \
  --disable-everything \
  --enable-protocol=file,pipe \
  --enable-demuxer=mp3,ogg,wav,flac,aac,mov,aiff \
  --enable-parser=mpegaudio,vorbis,opus,aac,flac \
  --enable-decoder=mp3,mp3float,mp2,mp2float,vorbis,opus,aac,aac_fixed,flac,alac \
  --enable-decoder=pcm_s16le,pcm_s16be,pcm_u8,pcm_s24le,pcm_s32le,pcm_f32le,pcm_f64le,adpcm_ms,adpcm_ima_wav \
  > "$OUT/configure.log" 2>&1 || { tail -30 "$OUT/configure.log"; exit 1; }
make -j"$(nproc)" > "$OUT/make.log" 2>&1 || { tail -40 "$OUT/make.log"; exit 1; }
make install > /dev/null
ls -la "$OUT/lib"
