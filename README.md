# ounsu!

**ounsu!** is an experimental, **unofficial** port of [osu! lazer](https://github.com/ppy/osu) (C#/.NET) to a jailbroken Switch 2, as a homebrew `.nro` under [kaiHEN](https://github.com/ouncebrew/kaihen). It starts from hbmenu and runs natively on the console from its SD card.

> [!IMPORTANT]
> ounsu! is an unofficial fan project. It is not affiliated with or endorsed by peppy, ppy Pty Ltd, or anyone on the osu! team. "osu!" and "ppy" are ppy's trademarks. Do not report ounsu! problems to ppy. You can support the official osu! project at https://osu.ppy.sh/home/support.

> [!CAUTION]
> ounsu! connects to the official osu! servers, and especially as **this project is like 99% vibecoded**, I am not responsible for anything that happens to your account by using it.
>
> No attempt is made to circumvent anti-cheat and/or enable features like score submission and multiplayer. No online function that ounsu! has is anything beyond what compiling the game in release mode on PC allows.

osu! and osu-framework are compiled ahead of time with .NET NativeAOT. All of the game's code is linked into one object that talks Linux syscalls: the NativeAOT runtime, SDL3, Realm, SQLite, OpenSSL, FFmpeg, veldrid-spirv and musl libc. A libounce host program runs that object and answers its syscalls with the console's services: SD card files, sockets, threads, audio and input. Rendering uses Veldrid's Vulkan backend on OunceVK.

## Status

The game runs and plays on kaiHEN: started from hbmenu, entirely from the console and its SD card, and back to hbmenu when you quit.

Currently working:
  - All main gameplay features and screens like main menu, song select, gameplay, and results
  - Various control schemes (controller, touchscreen, gyro, USB keyboard and mouse, **and Joy-Con 2 mouse mode**)
  - Rendering through OunceVK at 60 or 120 Hz, at the console's resolution in handheld mode (1080p) and the TV output resolution when docked (up to 4K), switching live when you dock or undock
  - Low latency audio
  - Networking features: login, beatmap downloads, leaderboards, and dashboard
    - Note: as this is a custom build of the game and does not include the game's anti-cheat, score submission, multiplayer, spectating, and online status are all (and will always be) completely disabled
  - No garbage collections during gameplay to ensure best performance. There are still pauses of 100 to 500 ms outside gameplay (song select, map loading, leaving a map)
  - A "Switch 2" section in osu!'s settings

Not working yet:
  - Drawing tablets aren't supported, not sure if they ever will be

The progress log is in `FEASIBILITY.md` and the hardware tests are in `TESTING.md`. Moving to a newer osu! release: `UPDATING.md`.

## Getting started

You need a Switch 2 running [kaiHEN](https://github.com/ouncebrew/kaihen): follow its documentation (and [ns2jb](https://gitlab.com/kurumuz/ns2jb)'s README) until you can get to hbmenu by holding **R** while starting a Switch 2 game.

### Binary release

1. Download `ounsu-<version>.zip` from the [latest release](../../releases/latest). It holds the game (`switch/ounsu.nro`) and osu!'s resources (`switch/ounsu/osu-resources/`).
2. Extract it to the root of your SD card, merging its `switch` folder with the one already there. You can put the card in your PC, or use ounceshell's FTP server.
3. Start hbmenu and choose **ounsu!**. The first start shows osu!'s first-run setup; your data (database, beatmaps, skins, settings) is created in `/switch/ounsu/home/.local/share/osu/`.

To update, delete `/switch/ounsu/osu-resources/`, then extract the new zip the same way. Your osu! data isn't touched.

### From source

You need Linux (x86_64), the [ounce-devkit](https://github.com/ouncebrew/ounce-devkit) (devkitPro with libounce and OunceVK), the .NET 10 SDK, Python 3, and cmake, ninja, make, perl, curl and unzip.

1. **The musl toolchain.** Build the `aarch64-linux-musl` cross toolchain with its syscall patch and link it as `toolchain/` (commands in [`patches/musl.md`](patches/musl.md)).
2. **The external sources.** `sh tools/fetch-sources.sh` checks out osu!, osu-framework, mysoundlib and the native libraries into `src/` at the commits in `patches/*.base` and applies `patches/*.diff` (several GB).
3. **The native libraries.** `sh tools/build-native.sh` builds SDL3, SQLite, OpenSSL, FFmpeg, veldrid-spirv and Realm as static musl libraries (`src/native/*/build-lx`).
4. **The CA bundle.** HTTPS needs a CA certificate bundle at `fsroot/usr/local/ssl/cert.pem`; the build puts it into the `.nro`. Your system's bundle works, e.g. `mkdir -p fsroot/usr/local/ssl && cp /etc/ssl/certs/ca-certificates.crt fsroot/usr/local/ssl/cert.pem`.
5. **Build:**

   ```sh
   sh console/osu-game/build.sh   # full build (NativeAOT compile, about 15 minutes)

   # or

   LX_SKIP_PUBLISH=1 sh console/osu-game/build.sh   # relink only, after changing the C side (about a minute)
   ```

   The result is `console/osu-game/build/ounsu.nro`.

6. **Resources.** Extract osu!'s resources into loose files from the `ppy.osu.Game.Resources` NuGet package. The full build has already downloaded the version osu! uses, and these commands find it:

   ```sh
   R=$(sed -n 's/.*"ppy\.osu\.Game\.Resources" Version="\([^"]*\)".*/\1/p' src/osu/osu.Game/osu.Game.csproj)
   DLL=$(ls "${NUGET_PACKAGES:-$HOME/.nuget/packages}"/ppy.osu.game.resources/$R/lib/*/osu.Game.Resources.dll)
   dotnet run --project tools/resource-extract -c Release -- extract \
       fsroot/osu-resources console/osu-game/osu-resources.substitutions.xml "$DLL"
   ```

7. **Copy to the SD card:** `ounsu.nro` to `/switch/ounsu.nro`, and `fsroot/osu-resources` to `/switch/ounsu/osu-resources/`. With ounceshell's FTP server running, `python3 tools/ftp-upload.py fsroot/osu-resources /switch/ounsu/osu-resources --host <console IP>` does the second part.

For development, hbmenu's netloader (press **Y**) and `nxlink -s -a <console IP> console/osu-game/build/ounsu.nro` send a build and show its log on the PC. The netloader saves the build it receives as `/switch/ounsu.nro`.

### What goes where on the SD card

| Path | What |
|---|---|
| `/switch/ounsu.nro` | The game |
| `/switch/ounsu/osu-resources/` | osu!'s resources (from the release zip, or extracted with `tools/resource-extract`) |
| `/switch/ounsu/home/.local/share/osu/` | osu!'s data folder (created on first start: database, beatmaps, skins, settings) |
| `/switch/ounsu/import/` | Drop beatmaps, skins and replays here to import them (see below) |
| `/switch/ounsu/lx-env` | Optional: extra environment variables, one `KEY=VALUE` per line. `LX_LOG_FILE=/lx-last.log` keeps a copy of the log on the card |

`/switch/ounsu` is the game's file system root ("/"). The CA certificates for HTTPS are built into the `.nro`.

## How it works

osu! and osu-framework are compiled ahead of time with .NET NativeAOT for `linux-musl-arm64`. Everything the game needs (osu!, the NativeAOT runtime, SDL3, Realm, SQLite, OpenSSL, FFmpeg, veldrid-spirv, libstdc++ and musl libc) is linked into one relocatable object, the **island** (`tools/aot-island.py`, `tools/make-island.sh`). The island has no imports: its only export is `lx_island_main`, and it reaches the outside world only through a table of function pointers (`struct lx_host`, `lx/include/lx_abi.h`): Linux syscalls, thread creation, logging, Vulkan and the console's input, audio and display.

The **host** (`lx/host`) is an ordinary libounce `.nro`. It implements the Linux syscalls the island makes on top of libnx: files on the SD card, memory, threads and futexes, sockets, epoll and DNS. It also gives the island OunceVK's Vulkan, reads the controllers, touchscreen, keyboard and mouse, and plays audio through audout. On exit it ends the island's threads, cleans up the Vulkan objects and frees the memory, so hbmenu comes back cleanly.

`lx/dotnet` holds the NativeAOT-specific parts (delegate thunks, Vulkan glue). `console/osu-game/shim` is osu!'s side of the boundary: input turned into SDL events, low-latency audio output, GC tuning and stubs for BASS, which osu-framework references but this build doesn't use.

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

The buttons send ordinary keyboard keys, so osu!'s key binding settings can use any of them. A Joy-Con lifted off the surface is an ordinary controller again.

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

Beatmaps can be downloaded in-game once you're logged in. You can also put `.osz`/`.olz` beatmaps, `.osk` skins and `.osr` replays in `/switch/ounsu/import/` on the SD card, and osu! will import them at startup. You can also add them using ounceshell's FTP server, and osu! keeps checking for new files while it runs (disabled during gameplay). Imported beatmap archives are deleted as on desktop; skins and replays move to `imported/`, and files that failed to import to `failed/`.

## Layout

| Path | What |
|---|---|
| `console/osu-game/` | The osu! app: `Program.cs` (entry point, import folder, self-tests), `Switch2Settings.cs`, `GameplayGcSession.cs` (no GCs during gameplay), `HubSelfTest.cs`, `osu-game.csproj`, `build.sh`, `rd.xml`, `shim/` (input, audio output, GC, BASS stubs) |
| `lx/host/` | The libounce host program: syscalls, files, memory, threads, networking, Vulkan, input, audio, display |
| `lx/island/`, `lx/dotnet/`, `lx/include/` | The island's side of the boundary (musl hooks, NativeAOT thunks and Vulkan glue) and the shared ABI header |
| `probes/` | Bring-up tests for the runtime, one per step: `hello-island` (a musl program on the host), `aot-hello` (NativeAOT), `vk-island` (Vulkan), `gfx-probe` (Veldrid), `net-probe` (sockets, DNS, TLS, HTTP, WebSocket), `hub-probe` (osu!'s metadata hub against a local server) |
| `patches/` | The changes to the external projects, their base commits and notes (see below), and the musl toolchain recipe |
| `tools/` | `fetch-sources.sh`, `build-native.sh` (+ `musl-toolchain.cmake`), `aot-island.py` and `make-island.sh` (island link), `capture-link.sh`, `gen-ssl-syms.py`, `gen-vk-tramp.py`, `scrub-paths.py`, `resource-extract` (osu-resources → loose files), `gen-rdxml` (rd.xml generator), `export-patches.sh`, `release.sh` + `gen-notices.py`, `ftp-upload.py`, `net-probe-server.py` |

Not tracked (large, external or private): `src/` (the osu!, osu-framework, mysoundlib and native library checkouts),
`toolchain/`, `fsroot/` (game resources and osu! data), build outputs.

## External checkouts and patches

| Patch | Checkout | Contents |
|---|---|---|
| `patches/osu.diff` | `src/osu` | build-date version stamp; `#if OUNSU` changes: AutoMapper-free Realm detach, loose-file resources, gameplay clock tweaks, metadata hub over a NativeAOT-safe protocol, two downloads at a time, notifications-connector null fix |
| `patches/osu-framework.diff` | `src/osu-framework` | PR #6002 (SDL3 audio) merged; `#if OUNSU`: window size and refresh rate follow the host's display (resolution per console mode, 60/120 Hz), windowed-only mode, NativeAOT fixes, BASS stub fixes, 8 connections per server; `#if OUNSU_AUDIO`: SDL3 audio, FFmpeg.AutoGen 7.1.1 |
| `patches/mysoundlib.diff` | `src/mysoundlib` | compiled into the app, no BASS/ASIO, 16-bit segmented track storage, mixer buffer fix |
| `patches/realm-dotnet.diff` | `src/native/realm-dotnet` | static wrappers; realm-core under `OUNSU_ISLAND`: `File::MapBase::try_reserve` disabled, the generic commit helper wakes its listener |
| `patches/veldrid-spirv.diff`, `patches/openssl.diff` | `src/native/...` | static build; OpenSSL reads the CPU features with `getauxval` |
| `patches/SDL.diff`, `patches/ffmpeg.diff` | `src/native/...` | none (unmodified) |
| `patches/musl-hbx-syscall-hooks.diff` | the musl toolchain | every syscall goes through a hook the island implements (`patches/musl.md`) |

Each `patches/<name>.md` describes the changes; each `*.base` names the commit the patch applies to. Refresh the snapshots with `sh tools/export-patches.sh` before committing.

## Licence

ounsu!'s own code is under the [MIT licence](LICENSE). Parts of `lx/host` are ported from HBLauncher's Linux runtime, and `patches/musl-hbx-syscall-hooks.diff` is HBLauncher's (MIT, RavensClawX and contributors; its notice is in `LICENSE` too). The other files in `patches/` change other projects and are under those projects' licences. Release zips also contain osu!'s resources (CC BY-NC 4.0, non-commercial use only) and libraries under their own licences, all listed in `THIRD_PARTY_NOTICES.txt` inside the zip.

## Credits

**The game**
- [osu!](https://github.com/ppy/osu) and [osu-framework](https://github.com/ppy/osu-framework) by ppy (Dean Herbert) and contributors (MIT). [osu-resources](https://github.com/ppy/osu-resources) (CC BY-NC 4.0) supply textures, sounds, fonts and the logo. Release zips include them unmodified, under that licence (not this repository's): non-commercial use only. osu! is a trademark of ppy Pty Ltd; this port isn't affiliated with or endorsed by ppy.
- [osu-framework PR #6002](https://github.com/ppy/osu-framework/pull/6002) (SDL3 audio backend) and [mysoundlib](https://github.com/hwsmm/mysoundlib) (LGPL-2.1) by hwsmm: the audio in this port.

**Running code on the Switch 2**
- [ns2jb](https://gitlab.com/pepsipu/ns2jb), the Switch 2 exploit by Sammy Hajhamid (pepsipu), used through [kurumuz's fork](https://gitlab.com/kurumuz/ns2jb).
- [kaiHEN](https://github.com/ouncebrew/kaihen), [libounce](https://github.com/ouncebrew/libounce), [OunceVK](https://github.com/ouncebrew/ouncevk) and the [ounce-devkit](https://github.com/ouncebrew/ounce-devkit) (ouncebrew), built on [devkitPro](https://devkitpro.org/) and [libnx](https://github.com/switchbrew/libnx).
- [HBLauncher](https://github.com/ravensclawx/HBLauncher) by RavensClawX: the hbxl Linux runtime that the host program's syscall layer is ported from, and the musl syscall hooks. Also by RavensClawX, the [Twilight Princess](https://github.com/ravensclawx/TwilightPrincess-HBLauncher) and [Animal Crossing](https://github.com/ravensclawx/AnimalCrossing-HBLauncher) HBLauncher ports: the Vulkan vi-window approach and the musl cross-build setup.
- [WiiCompiled-Switch2](https://github.com/6rimson/WiiCompiled-Switch2) by 6rimson: many hardware findings, among them how the console's socket service behaves.

**Libraries compiled into the build**
- [.NET](https://github.com/dotnet/runtime) and NativeAOT (.NET Foundation, MIT).
- [SDL](https://github.com/libsdl-org/SDL) (zlib), [Veldrid](https://github.com/mellinoe/veldrid) by Eric Mellino ([ppy's fork](https://github.com/ppy/veldrid), MIT), [veldrid-spirv](https://github.com/ppy/veldrid-spirv) (MIT) with Khronos' SPIRV-Cross, shaderc and glslang (Apache 2.0), and the Khronos Vulkan headers.
- [FFmpeg](https://ffmpeg.org/) 7.1.1 (LGPL 2.1+, audio decoders only), [FFmpeg.AutoGen](https://github.com/Ruslan-B/FFmpeg.AutoGen) by Ruslan Balanukhin (LGPL-3.0), [SoundTouch.Net](https://github.com/owoudenberg/soundtouch.net) by Olaf Woudenberg (LGPL-2.1+), and [NAudio](https://github.com/naudio/NAudio) by Mark Heath (MIT).
- [Realm](https://github.com/realm/realm-dotnet) and realm-core (Apache 2.0), SQLite (public domain) via SQLitePCLRaw by Eric Sink, [OpenSSL](https://www.openssl.org/) (Apache 2.0).
- [Newtonsoft.Json](https://github.com/JamesNK/Newtonsoft.Json) by James Newton-King (MIT) and [ImageSharp](https://github.com/SixLabors/ImageSharp) by Six Labors (Six Labors Split License).
- [musl](https://musl.libc.org/) libc (MIT), built with [musl-cross-make](https://github.com/richfelker/musl-cross-make) by Rich Felker.

Nintendo Switch 2 is a trademark of Nintendo, which is not affiliated with this project.
