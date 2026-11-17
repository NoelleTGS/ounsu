# ounsu!

**ounsu!** is an experimental, **unofficial** port of [osu! lazer](https://github.com/ppy/osu) to a jailbroken Switch 2, as a homebrew `.nro` under [kaiHEN](https://github.com/ouncebrew/kaihen). It starts from hbmenu and runs natively on the console from its SD card.

> [!IMPORTANT]
> ounsu! is an unofficial fan project. It is not affiliated with or endorsed by peppy, ppy Pty Ltd, or anyone on the osu! team. "osu!" and "ppy" are ppy's trademarks. Do not report ounsu! problems to ppy. You can support the official osu! project at https://osu.ppy.sh/home/support.

> [!CAUTION]
> ounsu! connects to the official osu! servers, and especially as **this project is like 99% vibecoded**, I am not responsible for anything that happens to your account by using it.
>
> No attempt is made to circumvent anti-cheat and/or enable features like score submission and multiplayer. No online function that ounsu! has is anything beyond what compiling the game in release mode on PC allows.

The game runs as a custom Linux build compiled into a single native file using NativeAOT. It bundles everything (SDL3, SQLite, FFmpeg, .NET runtime, etc.) and talks directly to Linux system calls. A small host program on the console intercepts those system calls and maps them to the hardware (similar to translation layers like Wine or WSL 1). Graphics are handled using Veldrid's Vulkan backend running on OunceVK.

## Status

Currently working:
  - All main gameplay features and screens like main menu, song select, gameplay, and results
  - Various control schemes:controller, touchscreen, gyro, USB keyboard and mouse, **and Joy-Con 2 mouse mode** (check [#Controls](#Controls))
  - Renders at 60 or 120 Hz, at the console's resolution in handheld mode (1080p) and the TV output resolution when docked (up to 4K), and swaps automatically when you dock or undock
  - Low latency audio
  - Networking features: login, beatmap downloads, leaderboards, and dashboard
    - Note: as this is a custom build of the game and does not include the game's anti-cheat, score submission, multiplayer, spectating, and online status are all (and will always be) completely disabled
  - No garbage collections during gameplay to ensure best performance. There are still pauses of 100 to 500 ms outside gameplay (song select, map loading, leaving a map)
  - A "Switch 2" section in the in-game settings (see #)
  - Exiting game returns to hbmenu

Not working yet:
  - Drawing tablets aren't supported, and cannot be with the current exploits we have ([source from r/switch2hacks Discord](https://discord.com/channels/1358947330115502186/1358948865469976596/1571770606716846183))

## Getting started

You need a Switch 2 running [kaiHEN](https://github.com/ouncebrew/kaihen): follow its documentation (and [ns2jb](https://gitlab.com/kurumuz/ns2jb)'s README) until you can get to hbmenu by holding **R** while starting a Switch 2 game.

### Binary release

1. Download `ounsu-<version>.zip` from the [latest release](../../releases/latest).
2. Extract it to the root of your SD card, merging its `switch` folder with the one already there. You can put the card in your PC, or use ounceshell's FTP server.
3. Start hbmenu and choose **ounsu!**.
On your first boot, go through osu!'s first-run setup, and your data (database, beatmaps, skins, settings) is created in `/switch/ounsu/osu/`. You can also try moving data from a PC install of the game, though this is currently untested.

To update, delete `/switch/ounsu/osu-resources/`, then extract the new zip the same way.

### From source

You need Linux (x86_64), the [ounce-devkit](https://github.com/ouncebrew/ounce-devkit) (devkitPro with libounce and OunceVK), the .NET 10 SDK, Python 3, and cmake, ninja, make, perl, curl and unzip.

1. Build the `aarch64-linux-musl` cross toolchain with its syscall patch and link it as `toolchain/` (commands in [`patches/musl.md`](patches/musl.md)).
2. Clone the external sources and apply their patches:
    ```sh
    sh tools/fetch-sources.sh
    ```
3. Build all the libraries as static musl libraries:
    ```sh
    sh tools/build-native.sh
    ```
4. HTTPS needs a CA certificate bundle at `fsroot/usr/local/ssl/cert.pem`; the build puts it into the `.nro`. Your system's bundle works, e.g.
    ```sh
    mkdir -p fsroot/usr/local/ssl
    cp /etc/ssl/certs/ca-certificates.crt fsroot/usr/local/ssl/cert.pem
    ```
5. Build:
   ```sh
   sh console/osu-game/build.sh   # full build (NativeAOT compile)

   # or

   LX_SKIP_PUBLISH=1 sh console/osu-game/build.sh   # relink only, after changing the C side
   ```
   The result is in `console/osu-game/build/ounsu.nro`.

6. Extract osu!'s resources into loose files from the `ppy.osu.Game.Resources` NuGet package. The full build has already downloaded the version osu! uses, use these commands to find it:

   ```sh
   R=$(sed -n 's/.*"ppy\.osu\.Game\.Resources" Version="\([^"]*\)".*/\1/p' src/osu/osu.Game/osu.Game.csproj)
   DLL=$(ls "${NUGET_PACKAGES:-$HOME/.nuget/packages}"/ppy.osu.game.resources/$R/lib/*/osu.Game.Resources.dll)
   dotnet run --project tools/resource-extract -c Release -- extract \
       fsroot/osu-resources console/osu-game/osu-resources.substitutions.xml "$DLL"
   ```

7. Copy `ounsu.nro` to the SD card's `/switch/ounsu/ounsu.nro`, and `fsroot/osu-resources` to `/switch/ounsu/osu-resources/`. 
   - With ounceshell's FTP server running, you can use `python3 tools/ftp-upload.py fsroot/osu-resources /switch/ounsu/osu-resources --host <console IP>` to copy over the resources.

For development, open hbmenu's netloader (press **Y**) and use `nxlink -s -a <console IP> -p ounsu/ounsu.nro console/osu-game/build/ounsu.nro` send a build and show its log on the PC. The netloader saves the build it receives as `/switch/ounsu/ounsu.nro`.

### SD Card Layout

| Path | What |
|---|---|
| `/switch/ounsu/ounsu.nro` | The game |
| `/switch/ounsu/osu-resources/` | osu!'s resources (from the release zip, or extracted with `tools/resource-extract`) |
| `/switch/ounsu/osu/` | osu!'s data folder (created on first start: database, beatmaps, skins, settings) |
| `/switch/ounsu/import/` | Drop beatmaps, skins and replays here to import them (see below) |
| `/switch/ounsu/lx-env` | Optional: extra environment variables, one `KEY=VALUE` per line. `LX_LOG_FILE=/lx-last.log` keeps a copy of the log on the card |

`/switch/ounsu` is the game's file system root ("/").

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
| L / R                       | C / V                                             |
| L3 / R3 (stick clicks)      | A / S                                                                         |
| B (hold)                    | Default bind for the gyro cursor: tilt and turn the controller to move the cursor. B sends nothing to osu! unless you change the bind in-game.       |
| USB keyboard and mouse      | Work as on a PC (US keyboard layout)                                                        |
| Joy-Con 2 mouse mode        | Slide a Joy-Con on a surface: it moves the cursor, L/ZL or R/ZR click, its stick scrolls    |

The buttons send ordinary keyboard keys, so osu!'s key binding settings can use any of them. If you have Joy-Con mouse mode enabled, it will act as an ordinary controller if you lift it off the surface.

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

Beatmaps can be downloaded in-game once you're logged in. You can also put `.osz`/`.olz` beatmaps, `.osk` skins and `.osr` replays in `/switch/ounsu/import/` on the SD card, and osu! will import them at startup. You can also add them using ounceshell's FTP server, and osu! keeps checking for new files while it runs (disabled during gameplay). Imported beatmap archives are deleted as on desktop, skins and replays move to `imported/`, and files that failed to import to `failed/`.

## Licence

ounsu!'s own code is under the [MIT licence](LICENSE).

Parts of `lx/host` are ported from HBLauncher's Linux runtime, and `patches/musl-hbx-syscall-hooks.diff` is HBLauncher's (MIT, RavensClawX and contributors; its notice is in `LICENSE` too). The other files in `patches/` change other projects and are under those projects' licences. Release zips also contain osu!'s resources (CC BY-NC 4.0, non-commercial use only) and libraries under their own licences, all listed in `THIRD_PARTY_NOTICES.txt` inside the zip.

## Credits

**The game**
- [osu!](https://github.com/ppy/osu) and [osu-framework](https://github.com/ppy/osu-framework) by peppy (Dean Herbert) and contributors (MIT). [osu-resources](https://github.com/ppy/osu-resources) (CC BY-NC 4.0) supply textures, sounds, fonts and the logo. Release zips include them unmodified under its licence.
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

Nintendo Switch 2 is a trademark of Nintendo. osu! and ppy are trademarks of ppy Pty Ltd.
