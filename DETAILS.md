All the Claude slop from README.md has been moved into this file to make that thing not suck ass to read.

The progress log is in `FEASIBILITY.md` and the hardware tests are in `TESTING.md`. Moving to a newer osu! release: `UPDATING.md`.

## How it works

osu! and osu-framework are compiled ahead of time with .NET NativeAOT for `linux-musl-arm64`. Everything the game needs (osu!, the NativeAOT runtime, SDL3, Realm, SQLite, OpenSSL, FFmpeg, veldrid-spirv, libstdc++ and musl libc) is linked into one relocatable object, the **island** (`tools/aot-island.py`, `tools/make-island.sh`). The island has no imports: its only export is `lx_island_main`, and it reaches the outside world only through a table of function pointers (`struct lx_host`, `lx/include/lx_abi.h`): Linux syscalls, thread creation, logging, Vulkan and the console's input, audio and display.

The **host** (`lx/host`) is an ordinary libounce `.nro`. It implements the Linux syscalls the island makes on top of libnx: files on the SD card, memory, threads and futexes, sockets, epoll and DNS. It also gives the island OunceVK's Vulkan, reads the controllers, touchscreen, keyboard and mouse, and plays audio through audout. On exit it ends the island's threads, cleans up the Vulkan objects and frees the memory, so hbmenu comes back cleanly.

`lx/dotnet` holds the NativeAOT-specific parts (delegate thunks, Vulkan glue). `console/osu-game/shim` is osu!'s side of the boundary: input turned into SDL events, low-latency audio output, GC tuning and stubs for BASS, which osu-framework references but this build doesn't use.

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
