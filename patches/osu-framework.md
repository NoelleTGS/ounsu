# osu-framework (`src/osu-framework`)

Upstream: [ppy/osu-framework](https://github.com/ppy/osu-framework), tag `2026.1030.0` (`osu-framework.base`). Patch:
`osu-framework.diff`. `src/osu` references this checkout instead of the NuGet package.

## 1. PR #6002 merged (SDL3 audio)

[osu-framework PR #6002](https://github.com/ppy/osu-framework/pull/6002) by hwsmm, merged onto the tag: an audio backend on
SDL3 + FFmpeg (through [mysoundlib](mysoundlib.md)) instead of BASS, which has no console build. These files come from it:
`Audio/SDL3AudioManager.cs`, `Audio/SDL3AudioDecoderQueue.cs`, `Audio/Mixing/SDL3/*`, `Audio/Sample/*SDL3*`,
`Audio/Track/TrackSDL3.cs`, `Audio/Callbacks/SDL3AudioFileCallbacks.cs`, `Audio/BassAudioManager.cs`,
`Configuration/AudioDriver.cs`, the `osu.Framework.Tests/Audio` changes, and the related AudioManager/Sample/Track edits.
On top of it: FFmpeg.AutoGen 7.1.1 (matches the static FFmpeg 7.1.1), and the `HbxlAudio` build property
(`-p:HbxlAudio=true`, defines `HBXL_AUDIO`).

## 2. `#if HBXL` (console build)

| File | Change |
|---|---|
| `Platform/SDL3/SDL3Window.cs` | SDL runs on its offscreen video driver; the real display is HBLauncher's vi window, handed to Vulkan as a fake "Wayland surface" (the shim in `console/osu-game/shim/hbxl_vk_shim.c` maps it onto `VK_NN_vi_surface`) |
| `Platform/SDL3/SDL3Window.Windowing.cs` | Window pinned to 1920x1080 and windowed mode only (the console only shows the first swapchain, and only at 1920x1080) |
| `Platform/GameHost.cs` | Vulkan only, no OpenGL |
| `Platform/HbxlNative.cs` (new) | Bindings to the hbxl runtime's exports |
| `Platform/Linux/Native/Library.cs`, `Threading/AudioThread.cs` | No dlopen search paths: everything is linked statically |
| `Timing/ThrottledFrameClock.cs` | A thread that skips its frame sleep still passes a GC safepoint (no signals on hbxl, so the GC can't interrupt a busy thread; this was a hardware hang) |
| `Graphics/Veldrid/Vertices/VeldridVertexUtils.cs` | Vertex attributes in byte-offset order (NativeAOT's field order can differ) |
| `Graphics/Rendering/Renderer.cs` | Skips a layout self-check that needs marshalling metadata NativeAOT lacks |
| `Graphics/Shaders/ShaderCompilationStore.cs` | Rejects shader-cache entries written by broken builds (they crashed the GPU driver) |
| `Bindables/Bindable.cs`, `Bindables/INotifyValueChangedUntyped.cs` | Non-generic value subscription (no `MakeGenericMethod` over value types) |
| `Audio/Track/TrackSDL3.cs`, `Audio/SDL3AudioManager.cs`, `Configuration/FrameworkConfigManager.cs` | Track reads from the file server don't stall the audio thread; mysoundlib is compiled in; SDL3 is the only audio driver |
| `Audio/Track/TrackBass.cs` | Copes with the BASS stubs (every call returns 0) |
| `IO/Network/WebRequest.cs` | At most 8 connections per server: HTTP/1.1 uses one socket per request, and SmileBASIC's socket service is shared with the file server. Also gentler on ppy's servers |
| `osu.Framework.csproj` | HBXL/HbxlAudio build properties |

## Restoring

```sh
git -C src/osu-framework checkout -f f05a31a9d82972e9b752060370ec17df17bdabda
git -C src/osu-framework apply --whitespace=nowarn ~/osu-HBLauncher/patches/osu-framework.diff
```
