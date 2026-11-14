# mysoundlib (`src/mysoundlib`)

Upstream: [hwsmm/mysoundlib](https://github.com/hwsmm/mysoundlib) (LGPL-2.1), base in `mysoundlib.base`. Patch:
`mysoundlib.diff`.

mysoundlib is the audio library behind osu-framework PR #6002: FFmpeg decoding, mixing, effects and tempo/pitch, written
in C# and normally shipped as a separate NativeAOT shared library (`libmysoundlib.so`) with C# bindings. The console has
no dynamic loading, so this port compiles both halves straight into the osu! binary.

## Changes

| File | Change |
|---|---|
| `hbxl/mysoundlib-hbxl.csproj` (new) | Builds the bindings and the library as one assembly referenced by osu-game. Defines `HBXL_NO_BASS` (no BASS/ASIO paths) and `HBXL_IN_PROCESS` (one .NET runtime instead of two). osu-game binds its `msl*` exports with DirectPInvoke |
| `bindings/MySoundLibrary.cs` | No `[SuppressGCTransition]` in-process: the callee is managed code and needs the GC transition (with it, the GC could deadlock) |
| `library/NativeAudioLibrary.cs` | SDL3 comes from osu-framework's own binding (one binary) |
| `library/SegmentedPcm.cs` (new), `library/Decoder.cs`, `library/TrackSDL3AudioPlayer.cs` | Decoded songs are stored as 16-bit PCM in 64 KiB segments. Upstream kept the whole decoded song in one array and re-allocated and copied the whole song whenever decoding ran past FFmpeg's duration estimate; on the console's small heap, those big arrays fragmented memory and crashed the game after a few songs |
| `library/Mixer.cs` | Bug fix: the buffer-size check was inverted, so the mix buffer shrank and never grew, and a larger SDL request (44.1 → 48 kHz resampling varies it) overflowed |

The Mixer fix is a plain upstream bug; worth reporting to mysoundlib.

## Restoring

```sh
git -C src/mysoundlib checkout -f 4359b0e05e19cd81729dde384dbb063186f2d10d
git -C src/mysoundlib apply --whitespace=nowarn ~/osu-HBLauncher/patches/mysoundlib.diff
```
