# osu! (lazer) → Switch 2 via kaiHEN

Goal: run osu! lazer (C#/.NET, ~/osu) as **homebrew (.nro) under kaiHEN改変** on a jailbroken Switch 2: launched
from hbmenu, running entirely off the console and its SD card once started (no PC tether, no file server).
No game data is shipped; the user builds everything from source.

This project is a **port of the working HBLauncher version** (`~/osu-HBLauncher`, still intact, at `c11053a`). There,
osu! was playable with audio, online features, unplugged play and an in-game "Switch 2" settings section. Its
CLAUDE.md (`~/osu-HBLauncher/CLAUDE.md`) holds every hbxl-era finding in
detail. Read it before you assume something has to be rediscovered. `FEASIBILITY.md` (dated log: append, don't
rewrite), `TESTING.md` and `README.md` were restarted for kaiHEN on 2026-11-13; the hbxl versions are
in `~/osu-HBLauncher` (`git -C ~/osu-HBLauncher show c11053a:<file>`).

**Keep the repo lean (user's wish):** remove HBLauncher/hbxl-only material instead of carrying it along (SmileBASIC,
file server, offline mirror, PC-link UI, UDP diagnostics, range mapping). The game runs **fully disconnected by
default**; nxlink is only a development log channel. Never design features around a PC connection.

## Status (2026-11-13)

- **Plan approved: the "musl island"** (reasoning in FEASIBILITY.md, 2026-11-13). Everything osu! needs stays musl-built
  as on hbxl: the ILC object, NativeAOT libs, SDL3/FFmpeg/OpenSSL/Realm/veldrid-spirv/mysoundlib, the C shims and musl
  libc.a. It is linked `ld -r` into one `island.o`, and every symbol is hidden except an export list. Its only imports are
  `__lx_syscall` (patched musl syscall hook), `vkGetInstanceProcAddr` (OunceVK, via the island's fake dlopen) and a few
  host calls. A libounce `.nro` (`console/lx/`, to be written) implements the Linux syscalls on libnx. This is a port of
  hbxl's runtime.c/memory.c/thread.c/vfs.c/net.c, minus the file server.
  - Phase 0: devkit + vulkan-clear over nxlink; read libounce for: ~~TLS register~~ (**done: `tpidr_el0` is unused by
    the whole devkit**, which uses `tpidrro_el0`+0x1F8 via `-mtp=soft`), exception resume (**no: CPU exceptions never reach the app on
    20261114c either**, Test 0b; faults stay fatal as on hbxl, Phase 5 parked; see FEASIBILITY.md), heap size, JIT mapping
    addresses, threads left running at exit.
  - **Phase 1 DONE 2026-11-13** (Test 1, 2022/2022 checks on hardware): `lx/island` (musl side), `lx/host` (libounce
    .nro: syscalls, mmap, clone/futex), `tools/make-island.sh`, `probes/hello-island`. Process memory: 9152 MiB total.
    Phase 2: aot-hello stages 0-6 (thunks: prebuilt block vs JIT API).
    Phase 3: Vulkan, audout, pad/hid, bsd sockets. Phase 4: osu-game nro + SD layout + clean exit. Phase 5 (faults →
    managed exceptions) is parked: the loader never gets the exceptions.
- HBLauncher-only material was left out of this repo on 2026-11-13: bti-probe, vfs-hosttest, the net host tests, the
  HBLauncher/ns2jb patches, the file-server/UDP/unplug tools, hbxl-env, hbxl_sb_input.c, symbolize.py, the generated
  hbxl-build manifests (they had absolute home paths; `~/osu-HBLauncher` has them), and the PC-link code in
  Program.cs/Switch2Settings.cs. `make-manifest.py` and `make-base.py` stay until Phase 1's island link replaces them
  (make-base.py lists every static library osu! links).
- **Devkit installed** 2026-11-13: ounce-devkit 20261114 (libounce 9382b1e, OunceVK f5afcd2, kaiHEN eb81e19) in
  `/opt/devkitpro`. Arch's pacman needs both `[dkp-libs]` and `[dkp-linux]` repos.
- **For later: mouse controls.** kaiHEN runs homebrew inside a *Switch 2* game, not a Switch 1 title, so Joy-Con 2 mouse
  mode (dead end on HBLauncher) may deliver input now. Check libounce's hid API for it once input works (Phase 3+).

## Who does what

- The user runs every console test. Claude can't touch the hardware but can read any log on this PC: never ask the
  user to paste logs, read them.
- **Seeing the screen:** the user has a Genki ShadowCast capture card, **found by name** with `v4l2-ctl --list-devices`.
  Device numbers change, and another /dev/video device is the user's C920 webcam, so never grab a frame without
  checking which one is the ShadowCast. MJPEG 1920x1080, and the first frames can be empty. OBS often holds the device
  ("Device or resource busy"): ask the user to release it. Grab a frame:
  `ffmpeg -f v4l2 -input_format mjpeg -video_size 1920x1080 -i /dev/videoN -vf "select=gte(n\,10)" -frames:v 1 -update 1 -y captures/now.jpg`
  and view it with Read.

## The new platform: what's different from HBLauncher

| | HBLauncher (old) | kaiHEN (new) |
|---|---|---|
| Process | App range-mapped into **SmileBASIC**, kept alive by ns2jb's RPC | HEN runs **hbloader in a game process**; hbloader runs hbmenu, then the chosen .nro, and returns to hbmenu on exit |
| ABI / libc | "hbxl": Linux-syscall personality, **musl**, `aarch64-linux-musl` toolchain (`toolchain-src/`) | **libounce** (libnx-compatible) + devkitPro **newlib**, `aarch64-none-elf` (devkitA64) |
| Build | `hbxl-build.py` + manifest | libnx Makefile with `include $(DEVKITPRO)/libounce/switch2_rules`, or CMake via `source $DEVKITPRO/switch2vars.sh; aarch64-none-elf-cmake` |
| Files | PC file server + offline mirror in RAM (vfs.c) | SD card (`sdmc:/`), standard libnx fs |
| GPU | Console's own nn Vulkan via `VK_NN_vi_surface` + many shim workarounds | **OunceVK** (Mesa-based Vulkan, EGL, GL, GLES; `-lvulkan` / `-lEGL`) |
| Exec memory | None: code mapped RX at load, mprotect a no-op | kaiHEN **JIT API**: `__nx_jit_map_code` / `__nx_jit_unmap_code` (devkit's `kaihen-jit.o`); libounce has a Jit API on top |
| Launch | PC + ns2jb RPC + HBLauncher session every run | `ns2jb/hen.cjs` once per console boot, then hold **R** while starting a game → hbmenu |
| Logs | file server stdout, hbl-session.log, UDP diag | **nxlink** (`nxlinkStdio()`; Kaizhar does this), or a log file on the SD card |

Notes on the table:
- Kaizhar's JIT notes (`~/switch2hax/kaizhar/hen/jit/hen_jit.c`, measured 2026-11-09): at most **16 code mappings per
  process**; a mapping can only be unmapped whole; while mapped, the code is not writable and its source pages must not
  be touched. NativeAOT's delegate thunks (hbxl needed a prebuilt thunk block for them) might be able to use this. Not
  verified.
- devkitPro's Switch portlibs that only use libc + libnx (e.g. **SDL 2**) work unchanged with libounce. osu-framework
  uses **SDL 3**, which has no libounce port yet as far as we know.
- `.NET NativeAOT` has no `aarch64-none-elf`/newlib target. How to host NativeAOT's output (built for `linux-musl-arm64`
  or `linux-bionic-arm64`) on top of libounce/newlib is **the central question of the port plan**. The hbxl runtime
  (`~/HBLauncher/sdk/linux/*.c` + osu-game's shims) shows which Linux syscalls/behaviour .NET, Realm, OpenSSL, FFmpeg and
  SDL needed. That list is reusable even though the code underneath changes.

### HBLauncher findings that probably DON'T carry over (re-check, don't assume)

Specific to SmileBASIC or hbxl: hiding input from SmileBASIC (`hbxl_sb_input.c`), thread stacks from the SDK heap, the
first-swapchain-only / 1920x1080-only / R8G8B8A8_SRGB swapchain rules, `vkGetDeviceProcAddr` returning bogus pointers,
the security-cookie move to `.data` (mprotect no-op), `MAP_FIXED` / mremap / `/proc` workarounds, the file server
and offline mirror, the session/relaunch problems, `HBXL_FS_HOST` linked into the app, and "NullReferenceException = hard
crash" (that came from hbxl having no signals; libnx has its own exception handling, so check how faults reach
NativeAOT). The nn::hid / six-axis / keyboard **SDK offsets** were for SmileBASIC's SDK; libnx has real APIs for these
(`padGetStickPos`, `hidGetSixAxisSensorStates`, `hidGetKeyboardStates`, `hidGetMouseStates`).

### Findings that DO carry over (platform-independent; details in the old CLAUDE.md)

- **Patched sources** (`src/`) and everything managed: osu-framework `#if HBXL` patches (windowed only, renderer
  order, PR #6002 audio via SDL3 + mysoundlib + FFmpeg), osu `HBXL_*` defines (settings section, hubs, notification
  connector null fix, build-date assembly versions).
- **Build gotchas:** `-p:HbxlBuild=true` must be on the dotnet command line (a global property), otherwise the framework
  patches silently vanish. osu-game's assembly version must stay **0.0.0** (1.0.0 = "deployed" build: legacy score
  JSON, Sentry reports to ppy). Linking real `-lstdc++` needs NativeAOT's `libstdc++compat.a` dropped.
- **NativeAOT + JSON:** `rd.xml` from `tools/gen-rdxml` (build.sh regenerates it); `OSU_HBXL_JSON_SELFTEST=1`.
- **Metadata hub:** `HbxlMessagePackHubProtocol` + `HbxlHubTypes` (stock SignalR protocol needs expression trees);
  self-tests `OSU_HBXL_HUB_SELFTEST`, `probes/hub-probe`. Only the metadata hub; spectator/multiplayer are never contacted.
- **GC:** HeapHardLimit (640 MiB) well below GCRegionRange (1 GiB); no GCConserveMemory; never allocate >85 KB arrays per
  audio track (LOH fragmentation; songs are decoded into 64 KiB segments).
- **Never put a repeating task on the game's scheduler** (`Scheduler.AddDelayed(..., repeat: true)`): "Too many timed
  tasks" crash. Periodic work goes in `Update()`.
- Realm needed `File::MapBase::try_reserve` → false (no MAP_FIXED) and `get_robust_list`; recheck under newlib.
- `System.Console` needs `pipe2`; NativeAOT needs `/dev/urandom`-style randomness, Guid.NewGuid reads it early.
- Desktop NativeAOT comparison build (see old CLAUDE.md, "Current state") is still the fastest way to isolate bugs.

## Rules (unchanged)

- `~/osu` (commit 28283cdbfd): **don't edit**. Our modifiable copies are `src/osu` and `src/osu-framework`.
- `src/native/SDL`: **SDL's CLAUDE.md forbids AI-generated code. Claude must not edit it.** Put SDL-side behaviour in
  the port's own code (wrappers + SDL's public API). Any SDL 3 platform backend for libounce has to follow this too.
- **Be gentle with ppy's servers:** load/concurrency tests go to a PC server; real requests are single and spaced out.
  Self-built clients are refused score submission, multiplayer, spectator and online status by design. **Never work
  around that** (version hash, etc.).
- Desktop osu-game runs: isolated temporary `XDG_DATA_HOME`, a timeout, and only after a successful build. The default
  data folder is the user's real lazer install, which is logged in.
- The user has approved building external native code (cmake/ninja/make), but the auto-mode classifier may still block a
  command. If it does, explain and let the user run it with `!`. Edit CLAUDE.md and memory with Edit/Write, not shell
  scripts.

## Directory map

This repo (`~/switch2hax/osu-kaihen`) is a **fresh git repository** (started 2026-11-13, meant for a new public GitHub
repo). It shares no history with `~/osu-HBLauncher`, which stays separate; files were copied from there at `c11053a`.
Everything tracked here is public: no personal data or absolute home paths. `.gitignore` is a whitelist: only the port's own sources, docs, tools and patch snapshots
are tracked.

| Path | What |
|---|---|
| `console/osu-game` | The osu! app: `Program.cs`, `ConsoleOsuGame`, `Switch2Settings.cs`, `HubSelfTest.cs`, `rd.xml`, `build.sh` and `shim/` (hbxl C shims: input, audio out, net, vk, thunks, thread stacks). Still the HBLauncher build; PC-link code removed. |
| `probes/` | Probe sources (aot-hello, gfx, net, hub), still built for hbxl. aot-hello's `tools/` (capture-link.sh, make-manifest.py) and gfx-probe's shims are what osu-game's build uses. |
| `src/osu-framework`, `src/osu`, `src/mysoundlib` | Patched checkouts, copied **with their uncommitted changes** (git working trees; `patches/*.diff` snapshot them via `tools/export-patches.sh`). Build outputs (bin/obj) weren't copied. |
| `src/native/{SDL,ffmpeg,veldrid-spirv,realm-dotnet,openssl,sqlite-dl}`, `src/veldrid-src` | Native sources with the hbxl patches; the `build-hbx/` and `build-desktop/` outputs weren't copied (they still exist in `~/osu-HBLauncher/src/native/*`). Under the musl-island plan they stay **musl builds** (not devkitA64): reuse or rebuild them with the musl toolchain. |
| `patches/` | Diff + base commit + notes per external checkout (SDL, osu, osu-framework, ffmpeg, openssl, realm-dotnet, mysoundlib, veldrid-spirv). The hbxl runtime diff is `~/osu-HBLauncher/patches/HBLauncher.diff` (applied in `~/HBLauncher`). |
| `tools/` | resource-extract (osu-resources → loose files), gen-rdxml, build-ffmpeg-hbx.sh, export-patches.sh, net-probe-server.py. |
| `fsroot/` | Copied game data, a starting point for the SD card layout: `osu-resources/` (extracted resources), `usr/local/ssl/cert.pem` (CA bundle), `home/.local/share/osu` (the console's osu! data folder: realm, beatmaps, settings, **a logged-in session**; treat it as the user's private data). Nothing in it is tracked. |
| `vk-include/` | Vulkan headers used with Arch's cross gcc (OunceVK installs its own). |

Not copied (still in `~/osu-HBLauncher`): `toolchain-src/` (musl GCC, 3.2 GB), build outputs (`console/osu-game/{bin,obj,app/build}`,
probe builds), `captures/`, logs (`fsroot/last-run.log`, `udp-diag.log`), `fsroot/{tmp,import}`, `import-backup/`.
Disk space is tight (~20 GB free on `/` on 2026-11-13).

### ~/switch2hax (siblings)

| Path | What |
|---|---|
| `kaiHEN-20261114c/` | kaiHEN release: `ro/main`, `hen/main`, `game/main`, `bootstrap.bin`, `hbmenu.nro`, `programs/ounceshell.nro`, `licenses/` |
| `ns2jb/` (`28f03b5`) | Exploit runner, now with `hen.cjs` to load kaiHEN. Its README covers setup |
| `ounce-devkit/` (`f347815`, version 20261114) | `install.sh` (adds libounce, OunceVK, libkaihen, CMake toolchain, `switch2vars.sh` to devkitPro), `Dockerfile`, `examples/` (vulkan-clear, gles-triangle, plugin-echo + client), `build/sources` (pinned libounce / ouncevk / kaihen commits) |
| `ounce-hbloader/`, `ounce-hbmenu/` | The loader (nx-hbloader based; same environment as nx-hbloader, extra cleanup after exit) and hbmenu (`sdmc:/hbmenu.nro`) |
| `ounceshell/` | VitaShell port (file manager, FTP server), a large C homebrew example (`switch2/`) |
| `Checkpoint/` | Save manager port: GLES 3 instead of deko3d (`switch/source/gfx_gles.cpp`, `HEN` block in `switch/Makefile`) |
| `dusklight/` (`ad781a8`) | **Dusklight (Twilight Princess decomp) on kaiHEN, CC0: copying code from it is fine.** `hen/platform/` (start-up/exit, nxlink + SD log, threads on all cores + 8 MiB stacks, cond-var exit hygiene, Vulkan leftovers, malloc lock, shader-cache write queue, pipe/poll, POSIX SD semantics), `hen/patches/sdl3-switch2.patch` (Switch 2 SDL3 on devkitPro's SDL 3.4.10 port). Lessons summarised in FEASIBILITY.md 2026-11-13 |
| `kaizhar/` | Azahar (3DS emulator) port: the best reference for a big C++ app. `hen/` = compat (missing libc/socket/OpenSSL bits), `platform/` (POSIX fixes, thread cores/stacks, `sys/mman.h` + `dlfcn.h` stubs), `jit/hen_jit.c` (code memory), `vktrace/`; `src/citra_switch/switch_platform.c` (nxlink log, sockets, exception → addr2line dump, system messages). GPLv2+: read it, don't copy from it |

kaiHEN's own docs for the JIT and plugin APIs are `JIT.md` and `PLUGINS.md` in kaiHEN's `libkaihen/`. They aren't in this
folder: kaiHEN's source is fetched by `ounce-devkit/build/make-devkit.sh` (pinned in `build/sources`), and `kaihen.h`
comes with the installed devkit.

## Console workflow (from ns2jb's README; not yet done for osu!)

1. Once per console boot: SmileBASIC in Direct mode, then on the PC `cd ~/switch2hax/ns2jb && node hen.cjs --dir
   ~/switch2hax/kaiHEN-20261114c [PC-IP]`. Follow the printed SmileBASIC steps (`LOAD "TXT:NS2"`, `RUN`, `REMOVE
   NSR,&H60000000,&H3FFFFFFF`, `V=START(...)`) until `HEN launch completed!`, then close SmileBASIC normally.
2. Hold **R** while starting a native Switch 2 game. On a fresh setup the first launch only captures the game's policy;
   close it and launch it again with R held to get hbmenu. Keep that game on internal storage.
3. Homebrew goes on the SD card as `/switch/<name>.nro` (FAT32 or exFAT). For development, hbmenu's netloader + `nxlink -s
   app.nro` should send a build and receive its stdio. That's standard libnx, not yet tried here.
- ns2jb's FS exploit is unstable (~20% success per try); after a failure, a full console reboot is the reliable reset.
- Network: PC `192.168.1.206` (enp42s0), console `192.168.1.177`, both wired to the home router.

## Devkit setup (done 2026-11-13)

devkitPro pacman (on Arch: devkitPro's pacman repos/keyring; **both** `[dkp-libs]` and `[dkp-linux]` in `/etc/pacman.conf`), then in `~/switch2hax/ounce-devkit`: `sh install.sh`
(default 20261114; it installs `switch-dev`, `switch-zlib`, `dkp-toolchain-vars` if missing). Alternatively use the Docker
image (`docker build -t ounce-devkit .`). Kaizhar's README pins devkit 20261113f and kaiHEN 20261113. The release here is
20261114c, so check for version mismatches if something odd happens.
