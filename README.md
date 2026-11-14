# osu! (lazer) on the Switch 2, via kaiHEN

An experimental port of [osu! lazer](https://github.com/ppy/osu) (C#/.NET) to a jailbroken Switch 2, as a homebrew
`.nro` under [kaiHEN](https://github.com/ouncebrew/kaihen): started from hbmenu, running entirely from the console and
its SD card, with no PC needed once it starts.

osu! and osu-framework are compiled ahead of time with .NET NativeAOT. All of the game's code is linked into one object
that talks Linux syscalls: the NativeAOT runtime, SDL3, Realm, SQLite, OpenSSL, FFmpeg, veldrid-spirv and musl libc.
A libounce host program runs that object and answers its syscalls with the console's services: SD card files,
sockets, threads, audio and input. Rendering uses Veldrid's Vulkan backend on OunceVK.

No game data or binaries are distributed: everything is built from source on the PC.

## Status (2026-11-14)

osu! runs and plays on kaiHEN: started from hbmenu, entirely from the console and its SD card, and back to hbmenu
when you quit. Working so far:

- Rendering through OunceVK at 60 or 120 Hz, at the console's resolution in handheld mode (1080p) and the TV output
  resolution when docked (up to 4K), switching live when you dock or undock.
- Audio with low latency and no crackles.
- Every input: controllers, touchscreen, gyro aiming, USB keyboard and mouse, and Joy-Con 2 mouse mode.
- Networking: sockets, DNS and HTTPS (OpenSSL) work, and osu! downloads its online data. Logging in and beatmap
  downloads worked in the HBLauncher version but haven't been tested on kaiHEN yet.
- No garbage collections during gameplay, so no hitches mid-map. There are still pauses of 100 to 500 ms outside
  gameplay (song select, map loading, leaving a map).
- A "Switch 2" section in osu!'s settings.

Not done yet: HOME menu, sleep and closing from the HOME menu are not handled, and USB drawing tablets aren't
supported. A crash (a CPU fault) ends the whole game process: kaiHEN doesn't pass exceptions to homebrew. The progress
log is in `FEASIBILITY.md` and the hardware tests are in `TESTING.md`. Moving to a newer osu! release: `UPDATING.md`.

An earlier version of this port ran on HBLauncher (SmileBASIC host, PC file server). That version lives in a separate
repository; this one started fresh.

What ppy's servers refuse to self-compiled clients, by design: score submission, your own online status, spectator and
multiplayer. This port doesn't try to get around that.

## How it works

osu! and osu-framework are compiled ahead of time with .NET NativeAOT for `linux-musl-arm64`. Everything the game
needs (osu!, the NativeAOT runtime, SDL3, Realm, SQLite, OpenSSL, FFmpeg, veldrid-spirv, libstdc++ and musl libc) is
linked into one relocatable object, the **island** (`tools/aot-island.py`, `tools/make-island.sh`). The island has
no imports: its only export is `lx_island_main`, and it reaches the outside world only through a table of function
pointers (`struct lx_host`, `lx/include/lx_abi.h`): Linux syscalls, thread creation, logging, Vulkan and the
console's input, audio and display.

The **host** (`lx/host`) is an ordinary libounce `.nro`. It implements the Linux syscalls the island makes on top of
libnx: files on the SD card, memory, threads and futexes, sockets, epoll and DNS. It also gives the island OunceVK's
Vulkan, reads the controllers, touchscreen, keyboard and mouse, and plays audio through audout. On exit it ends the
island's threads, cleans up the Vulkan objects and frees the memory, so hbmenu comes back cleanly.

`lx/dotnet` holds the NativeAOT-specific parts (delegate thunks, Vulkan glue). `console/osu-game/shim` is osu!'s side
of the boundary: input turned into SDL events, low-latency audio output, GC tuning and stubs for BASS, which
osu-framework references but this build doesn't use.

## Building

You need the [ounce-devkit](https://github.com/ouncebrew/ounce-devkit) (devkitPro with libounce and OunceVK), the .NET
10 SDK, an `aarch64-linux-musl` cross toolchain, and the native libraries built as static musl libraries: SDL3,
Realm, SQLite and veldrid-spirv (with the patches below), OpenSSL, and FFmpeg (`tools/build-ffmpeg.sh`). Point
`LX_MUSL_TOOLCHAIN` and `LX_NATIVE_BUILDS` at them. Then:

```sh
sh console/osu-game/build.sh                     # full build (NativeAOT compile, about 15 minutes)
LX_SKIP_PUBLISH=1 sh console/osu-game/build.sh   # relink only, after changing the C side (about a minute)
```

The result is `console/osu-game/build/osu.nro`. The game's resources come from
[osu-resources](https://github.com/ppy/osu-resources): extract them to loose files with `tools/resource-extract`.

## On the console

Start kaiHEN and get to hbmenu as its documentation describes (hold **R** while starting a Switch 2 game). Then put
these on the SD card (`tools/ftp-upload.py` mirrors a folder over FTP, for example with ounceshell's FTP server):

| Path | What |
|---|---|
| `/switch/osu.nro` | The game |
| `/switch/osu/osu-resources/` | osu!'s resources, extracted with `tools/resource-extract` |
| `/switch/osu/home/.local/share/osu/` | osu!'s data folder (created on first start: database, beatmaps, skins, settings) |
| `/switch/osu/import/` | Drop beatmaps, skins and replays here to import them (see below) |
| `/switch/osu/lx-env` | Optional: extra environment variables, one `KEY=VALUE` per line. `LX_LOG_FILE=/lx-last.log` keeps a copy of the log on the card |

`/switch/osu` is the game's file system root ("/"). The CA certificates for HTTPS are built into the `.nro`.

For development, hbmenu's netloader (press Y) and `nxlink -s -a <console IP> console/osu-game/build/osu.nro` send a
build and show its log on the PC. The netloader saves the build it receives as `/switch/osu.nro`.

## Controls

`console/osu-game/shim/lx_input.c` turns the console's input into the mouse, keyboard and touch input osu! expects.

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
| USB keyboard and mouse      | Work as on a PC (US keyboard layout)                                                        |
| Joy-Con 2 mouse mode        | Slide a Joy-Con on a surface: it moves the cursor, L/ZL or R/ZR click, its stick scrolls    |

The buttons send ordinary keyboard keys, so osu!'s key binding settings can use any of them. A Joy-Con lifted off the
surface is an ordinary controller again.

## The "Switch 2" settings section

| Subsection | Settings |
|---|---|
| Display | Resolution in handheld mode (1080p, 900p, 720p) and docked (Automatic, 4K, 1440p, 1080p, 720p); frame rate (Highest = 120 Hz with variable refresh rate where the screen supports it, or 60 fps) |
| Import | Import everything in the import folder now; import new files automatically, and how often to look |
| Gyro aiming | The button to hold (or another one, which then no longer sends its key), sensitivity, inverted axes |
| Joy-Con mouse | Which Joy-Cons may act as a mouse (off, left, right, both), sensitivity |
| USB keyboard and mouse | On/off, mouse sensitivity |
| Audio output | Buffer size and number of queued buffers (lower = less delay, raise them if the sound crackles) |
| Diagnostics | Log every garbage collection |

## Importing files

Beatmaps can be downloaded in-game once you're logged in. You can also put `.osz`/`.olz` beatmaps, `.osk` skins and
`.osr` replays in `/switch/osu/import/` on the SD card. osu! imports them at startup and keeps checking for new files
while it runs (not during gameplay). Imported beatmap archives are deleted as on desktop; skins and replays move to
`imported/`, and files that failed to import to `failed/`.

## Layout

| Path | What |
|---|---|
| `console/osu-game/` | The osu! app: `Program.cs` (entry point, import folder, diagnostics), `Switch2Settings.cs`, `GameplayGcSession.cs` (no GCs during gameplay), `HubSelfTest.cs`, `osu-game.csproj`, `build.sh`, `rd.xml`, `shim/` (input, audio output, GC, BASS stubs) |
| `lx/host/` | The libounce host program: syscalls, files, memory, threads, networking, Vulkan, input, audio, display |
| `lx/island/`, `lx/dotnet/`, `lx/include/` | The island's side of the boundary (musl hooks, NativeAOT thunks and Vulkan glue) and the shared ABI header |
| `probes/` | Bring-up tests, one per step: `hello-island` (a musl program on the host), `aot-hello` (NativeAOT), `vk-island` (Vulkan), `gfx-probe` (Veldrid), `net-probe` (sockets, DNS, TLS, HTTP, WebSocket), `hub-probe` (osu!'s metadata hub against a local server) |
| `patches/` | Snapshots of the local changes in the external checkouts (see below) |
| `tools/` | `aot-island.py` and `make-island.sh` (island link), `gen-ssl-syms.py`, `gen-vk-tramp.py`, `resource-extract` (osu-resources → loose files), `gen-rdxml` (rd.xml generator), `build-ffmpeg.sh`, `export-patches.sh`, `ftp-upload.py`, `net-probe-server.py` |

Not tracked (large, external or private): `src/` (the osu!, osu-framework, mysoundlib and native library checkouts),
`fsroot/` (game resources and osu! data), build outputs.

## External checkouts and patches

| Patch | Checkout | Contents |
|---|---|---|
| `patches/osu.diff` | `src/osu` | build-date version stamp; `#if HBXL` changes: AutoMapper-free Realm detach, loose-file resources, gameplay clock tweaks, metadata hub over a NativeAOT-safe protocol, two downloads at a time, notifications-connector null fix |
| `patches/osu-framework.diff` | `src/osu-framework` | PR #6002 (SDL3 audio) merged; `#if HBXL`: window size and refresh rate follow the host's display (resolution per console mode, 60/120 Hz), windowed-only mode, NativeAOT fixes, BASS stub fixes, 8 connections per server; `#if HBXL_AUDIO`: SDL3 audio, FFmpeg.AutoGen 7.1.1 |
| `patches/mysoundlib.diff` | `src/mysoundlib` | compiled into the app, no BASS/ASIO, 16-bit segmented track storage, mixer buffer fix |
| `patches/SDL.diff` | `src/native/SDL` | the `hbx` audio/joystick drivers from the Twilight Princess HBLauncher port (not modified here) |
| `patches/realm-dotnet.diff` | `src/native/realm-dotnet` | realm-core under `HBX_SWITCH2`: `File::MapBase::try_reserve` disabled, the generic commit helper wakes its listener |
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
