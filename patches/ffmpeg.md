# FFmpeg (`src/native/ffmpeg`)

Upstream: [FFmpeg](https://ffmpeg.org/) tag `n7.1.1` (`ffmpeg.base`). Patch: `ffmpeg.diff` (empty: no source changes).

FFmpeg decodes the music and hitsounds for mysoundlib (osu-framework PR #6002). The version matches FFmpeg.AutoGen 7.1.1.
`sh tools/build-native.sh ffmpeg` configures a static, position-independent, **audio-only** build (decoders, demuxers
and swresample osu! needs, no video, no programs) for the island into `build-lx/`; `ffmpeg-desktop` makes an x86_64
shared build in `build-desktop/` for desktop comparison runs.

As with OpenSSL, FFmpeg.AutoGen loads its libraries with dlopen; a generated table (`gen-ssl-syms.py --table ffmpeg` →
`obj/ffmpeg_syms.c`) answers it from the static libraries.

Video backgrounds would need the video decoders and swscale added to the build script.

## Restoring

```sh
git -C src/native/ffmpeg checkout -f db69d06eeeab4f46da15030a80d539efb4503ca8
sh tools/build-native.sh ffmpeg   # output: src/native/ffmpeg/build-lx
```
