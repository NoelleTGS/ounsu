# osu-framework (`src/osu-framework`)

Upstream: [ppy/osu-framework](https://github.com/ppy/osu-framework), tag `2026.1114.0` (`osu-framework.base`). Patch:
`osu-framework.diff`. `src/osu` references this checkout instead of the NuGet package.

## 1. PR #6002 merged (SDL3 audio)

[osu-framework PR #6002](https://github.com/ppy/osu-framework/pull/6002) by hwsmm, merged onto the tag: an audio backend on
SDL3 + FFmpeg (through [mysoundlib](mysoundlib.md)) instead of BASS, which has no console build. These files come from it:
`Audio/SDL3AudioManager.cs`, `Audio/SDL3AudioDecoderQueue.cs`, `Audio/Mixing/SDL3/*`, `Audio/Sample/*SDL3*`,
`Audio/Track/TrackSDL3.cs`, `Audio/Callbacks/SDL3AudioFileCallbacks.cs`, `Audio/BassAudioManager.cs`,
`Configuration/AudioDriver.cs`, the `osu.Framework.Tests/Audio` changes, and the related AudioManager/Sample/Track edits.
On top of it: FFmpeg.AutoGen 7.1.1 (matches the static FFmpeg 7.1.1), and the `OunsuAudio` build property
(`-p:OunsuAudio=true`, defines `OUNSU_AUDIO`).

## 2. `#if OUNSU` (console build)

`-p:OunsuBuild=true` defines `OUNSU`. The code runs on the island (see the README, "How it works"); the native entry
points it binds are implemented in `console/osu-game/shim/lx_osu.c`.

| File | Change |
|---|---|
| `Platform/SDL3/SDL3Window.cs` | SDL runs on its offscreen video driver. The real display is the console's window (the host's vi window), handed to Vulkan as a fake "Wayland surface": `lx/dotnet/lx_vk.c` turns that into a `VK_NN_vi_surface` on OunceVK. `followOunsuDisplay` runs once per frame |
| `Platform/SDL3/SDL3Window.Windowing.cs` | Windowed mode only (the window already covers the screen). The windowed size is pinned to the console window's size and follows it every frame (`OunsuNative.Display()`): docking, undocking and the "Switch 2" Display settings resize the SDL window and with it the swapchain. The display mode's refresh rate is the host's frame rate (60 or 120 Hz), so the frame limiters follow it |
| `Platform/OunsuNative.cs` (new) | Bindings to the console's window: `ViWindow()` and `Display()` (size, frame rate, change serial; the call also lets the host handle the console's applet messages) |
| `Platform/GameHost.cs` | Vulkan only, no OpenGL |
| `Platform/Linux/Native/Library.cs`, `Threading/AudioThread.cs` | No dlopen search paths: everything is linked statically |
| `Timing/ThrottledFrameClock.cs` | A thread that skips its frame sleep still passes a GC safepoint (the runtime has no signals to interrupt a busy thread for the GC; without it the game hung) |
| `Graphics/Veldrid/Vertices/VeldridVertexUtils.cs` | Vertex attributes in byte-offset order (NativeAOT's field order can differ) |
| `Graphics/Rendering/Renderer.cs` | Skips a layout self-check that needs marshalling metadata NativeAOT lacks |
| `Graphics/Shaders/ShaderCompilationStore.cs` | Rejects shader-cache entries written by broken builds (they crashed the GPU driver) |
| `Bindables/Bindable.cs`, `Bindables/INotifyValueChangedUntyped.cs`, `Configuration/InputConfigManager.cs` | Non-generic value subscription (no `MakeGenericMethod` over value types) |
| `Audio/Track/TrackSDL3.cs`, `Audio/SDL3AudioManager.cs`, `Configuration/FrameworkConfigManager.cs` | A track's first read and decode run on the decoder thread, not the audio thread, and FFmpeg reads through a 64 KiB buffer (each read is a syscall to the SD card); mysoundlib is compiled in; SDL3 is the only audio driver |
| `Audio/Track/TrackBass.cs` | Copes with the BASS stubs (every call returns 0) |
| `Graphics/Video/FFmpegExtensions.cs`, `Graphics/Video/VideoDecoder.cs` | Builds against FFmpeg.AutoGen 7 (`OUNSU_AUDIO`): removed pixel formats, and no custom library loader (FFmpeg comes from the static libraries through the island's fake libdl) |
| `IO/Network/WebRequest.cs` | At most 8 connections per server. HTTP/1.1 uses one socket per request, and the console's socket service has a small pool. Also gentler on ppy's servers |
| `osu.Framework.csproj` | `OunsuBuild` / `OunsuAudio` build properties |

## Restoring

```sh
git -C src/osu-framework checkout -f ff1ae763ae01a0e8b0efb2fc32f7b9f7c283c800
git -C src/osu-framework apply --whitespace=nowarn patches/osu-framework.diff
```
