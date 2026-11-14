# osu! (lazer) on the Switch 2, via kaiHEN

An experimental port of [osu! lazer](https://github.com/ppy/osu) (C#/.NET) to a jailbroken Switch 2, as a homebrew
`.nro` under [kaiHEN](https://github.com/ouncebrew/kaihen): started from hbmenu, running entirely from the console and
its SD card, with no PC needed once it starts.

osu! and osu-framework are compiled ahead of time with .NET NativeAOT. All of the game's code is linked into one object
that talks Linux syscalls: the NativeAOT runtime, SDL3, Realm, SQLite, OpenSSL, FFmpeg, veldrid-spirv and musl libc.
A libounce host program runs that object and answers its syscalls with the console's services: SD card files,
sockets, threads, audio and input. Rendering uses Veldrid's Vulkan backend on OunceVK.

No game data or binaries are distributed: everything is built from source on the PC.

## Status (2026-11-13)

The port has just started. Nothing runs on kaiHEN yet. The plan and progress are in `CLAUDE.md` and `FEASIBILITY.md`.

The earlier version of this port ran on HBLauncher (SmileBASIC host, PC file server). It was playable, with audio,
online features, unplugged play and an in-game "Switch 2" settings section. That version lives in a separate repository; this one
started fresh. Those results are what the kaiHEN port aims to
reach again.

What ppy's servers refuse to self-compiled clients, by design: score submission, your own online status, spectator and
multiplayer. This port doesn't try to get around that.

## Controls (carried over from the HBLauncher version)

`console/osu-game/shim/hbxl_input.c` turns the touchscreen and controllers into the input osu! expects.

| Input                       | Does in osu!                                                                                |
| --------------------------- | ------------------------------------------------------------------------------------------- |
| Touchscreen (handheld only) | Touch: tap buttons, drag to scroll, tap circles in-game (osu! applies its Touch Device mod) |
| Left stick                  | Moves the cursor                                                                            |
| Right stick                 | Scroll (mouse wheel)                                                                        |
| A                           | Left click                                                                                  |
| Y                           | Right click                                                                                 |
| X                           | Enter                                                                                       |
| Plus                        | Back (Esc)                                                                                  |
| Minus                       | F9                                                                                          |
| D-pad                       | Arrow keys                                                                                  |
| ZL / ZR                     | Z / X (osu!'s default hit keys)                                                             |
| L / R                       | C / V (free keys: bind them in osu!'s settings)                                             |
| L3 / R3 (stick clicks)      | A / S (free keys)                                                                           |
| B (hold)                    | Gyro cursor: tilt and turn the controller to move the cursor. B sends nothing to osu!       |

The buttons send ordinary keyboard keys, so osu!'s key binding settings can use any of them. USB keyboards and mice
are planned too. kaiHEN runs homebrew inside a Switch 2 game, so Joy-Con 2 mouse mode may work as well. That didn't
work on HBLauncher, which ran inside a Switch 1 title.

## Importing files

Beatmaps can be downloaded in-game once you're logged in. You can also put `.osz` beatmaps, `.osk` skins and `.osr`
replays in the import folder on the SD card. osu! imports them at startup and keeps checking for new files while it
runs. The "Switch 2" section of osu!'s settings has a button that imports everything right away. The SD card layout
will be documented once the host program exists.

## Layout

| Path | What |
|---|---|
| `console/osu-game/` | The osu! app: `Program.cs` (entry point, diagnostics), `Switch2Settings.cs`, `HubSelfTest.cs`, `osu-game.csproj`, `build.sh`, `rd.xml`, `shim/` (C shims: input, audio output, Vulkan, networking, BASS stubs) |
| `probes/` | Bring-up probes: `aot-hello` (NativeAOT), `gfx-probe` (Veldrid/Vulkan), `net-probe` (sockets, DNS, TLS, HTTP, WebSocket), `hub-probe` (osu!'s metadata hub against a local server) |
| `patches/` | Snapshots of the local changes in the external checkouts (see below) |
| `tools/` | `resource-extract` (osu-resources → loose files), `gen-rdxml` (rd.xml generator), `build-ffmpeg.sh`, `export-patches.sh`, `net-probe-server.py` |

Not tracked (large, external or private): `src/` (the osu!, osu-framework, mysoundlib and native library checkouts),
`fsroot/` (game resources and osu! data), build outputs.

## External checkouts and patches

| Patch | Checkout | Contents |
|---|---|---|
| `patches/osu.diff` | `src/osu` | build-date version stamp; `#if HBXL` changes: AutoMapper-free Realm detach, loose-file resources, gameplay clock tweaks, metadata hub over a NativeAOT-safe protocol, two downloads at a time, notifications-connector null fix |
| `patches/osu-framework.diff` | `src/osu-framework` | PR #6002 (SDL3 audio) merged; `#if HBXL`: vi window/swapchain handling, windowed-only mode, NativeAOT fixes, BASS stub fixes, 8 connections per server; `#if HBXL_AUDIO`: SDL3 audio, FFmpeg.AutoGen 7.1.1 |
| `patches/mysoundlib.diff` | `src/mysoundlib` | compiled into the app, no BASS/ASIO, 16-bit segmented track storage, mixer buffer fix |
| `patches/SDL.diff` | `src/native/SDL` | the `hbx` audio/joystick drivers from the Twilight Princess HBLauncher port (not modified here) |
| `patches/realm-dotnet.diff` | `src/native/realm-dotnet` | realm-core `File::MapBase::try_reserve` disabled under `HBX_SWITCH2` |
| `patches/ffmpeg.diff`, `patches/veldrid-spirv.diff`, `patches/openssl.diff` | `src/native/...` | static builds |

Each `patches/<name>.md` describes the changes; each `*.base` names the commit the patch applies to. Refresh the
snapshots with `sh tools/export-patches.sh` before committing.

## Credits

**The game**
- [osu!](https://github.com/ppy/osu) and [osu-framework](https://github.com/ppy/osu-framework) by ppy (Dean Herbert) and contributors (MIT). [osu-resources](https://github.com/ppy/osu-resources) (CC BY-NC 4.0) supply textures, shaders and the logo, extracted on your own PC and never shipped. osu! is a trademark of ppy Pty Ltd; this port isn't affiliated with or endorsed by ppy.
- [osu-framework PR #6002](https://github.com/ppy/osu-framework/pull/6002) (SDL3 audio backend) and [mysoundlib](https://github.com/hwsmm/mysoundlib) (LGPL-2.1) by hwsmm: the audio in this port.

**Running code on the Switch 2**
- [ns2jb](https://gitlab.com/pepsipu/ns2jb), the Switch 2 exploit by Sammy Hajhamid (pepsipu), used through [kurumuz's fork](https://gitlab.com/kurumuz/ns2jb).
- [kaiHEN](https://github.com/ouncebrew/kaihen), [libounce](https://github.com/ouncebrew/libounce), [OunceVK](https://github.com/ouncebrew/ouncevk) and the [ounce-devkit](https://github.com/ouncebrew/ounce-devkit) (ouncebrew), built on [devkitPro](https://devkitpro.org/) and [libnx](https://github.com/switchbrew/libnx).
- [HBLauncher](https://github.com/ravensclawx/HBLauncher) by RavensClawX: the hbxl Linux runtime that the host program's syscall layer is ported from. Also by RavensClawX, the [Twilight Princess](https://github.com/ravensclawx/TwilightPrincess-HBLauncher) and [Animal Crossing](https://github.com/ravensclawx/AnimalCrossing-HBLauncher) HBLauncher ports: the SDL3 `hbx` patch, the Vulkan vi-window approach and the patched musl toolchain setup.
- [WiiCompiled-Switch2](https://github.com/6rimson/WiiCompiled-Switch2) by 6rimson: the socket layer this port's networking grew from, and many hardware findings.

**Libraries compiled into the build**
- [.NET](https://github.com/dotnet/runtime) and NativeAOT (.NET Foundation, MIT).
- [SDL](https://github.com/libsdl-org/SDL) (zlib), [Veldrid](https://github.com/mellinoe/veldrid) by Eric Mellino ([ppy's fork](https://github.com/ppy/veldrid), MIT), [veldrid-spirv](https://github.com/ppy/veldrid-spirv) (MIT) with Khronos' SPIRV-Cross, shaderc and glslang (Apache 2.0), and the Khronos Vulkan headers.
- [FFmpeg](https://ffmpeg.org/) 7.1.1 (LGPL 2.1+, audio decoders only), [FFmpeg.AutoGen](https://github.com/Ruslan-B/FFmpeg.AutoGen) by Ruslan Balanukhin (LGPL-3.0), [SoundTouch.Net](https://github.com/owoudenberg/soundtouch.net) by Olaf Woudenberg (LGPL-2.1+), and [NAudio](https://github.com/naudio/NAudio) by Mark Heath (MIT).
- [Realm](https://github.com/realm/realm-dotnet) and realm-core (Apache 2.0), SQLite (public domain) via SQLitePCLRaw by Eric Sink, [OpenSSL](https://www.openssl.org/) (Apache 2.0).
- [Newtonsoft.Json](https://github.com/JamesNK/Newtonsoft.Json) by James Newton-King (MIT) and [ImageSharp](https://github.com/SixLabors/ImageSharp) by Six Labors (Six Labors Split License).
- [musl](https://musl.libc.org/) libc (MIT), built with [musl-cross-make](https://github.com/richfelker/musl-cross-make) by Rich Felker.

Nintendo Switch 2 is a trademark of Nintendo, which is not affiliated with this project.
