# osu! (lazer) → kaiHEN: progress log

A dated log of findings for the kaiHEN port. Append new entries; don't rewrite old ones. The HBLauncher/hbxl log
(2026-11-04 to 2026-11-11, every finding that made the earlier version playable) is in
the separate osu-HBLauncher repository (`FEASIBILITY.md` there).

## 2026-11-13: approach chosen

**How NativeAOT's linux-musl-arm64 output runs on libounce/newlib: as a "musl island".**

- The prebuilt NativeAOT libraries use the musl ABI. `libRuntime.WorkstationGC.a` imports ~130 libc symbols and
  `libSystem.Native.a` ~200, many with Linux struct layouts. Linking them against newlib would break silently (errno
  numbers, clock ids, pthread type sizes and struct layouts all differ).
- All native dependencies are already built for musl (`~/osu-HBLauncher/src/native/*/build-hbx`). SDL3 has no
  libounce backend, and Claude must not write SDL code.
- OunceVK uses `VK_NN_vi_surface` + `nwindowGetDefault()`, the same surface path as hbxl.

So: the ILC object, NativeAOT libs, native deps, C shims and musl libc are linked (musl toolchain, `ld -r`) into one
object whose only exports and imports are a syscall entry, `vkGetInstanceProcAddr` and a few host calls. A libounce
`.nro` implements the Linux syscalls on libnx. This is a port of the hbxl runtime, simplified: SD card instead of the
file server, libnx sockets/threads, futex via address arbitration.

Rejected: rebuilding everything for devkitA64/newlib. That would mean building the NativeAOT runtime and System.Native
from source, an SDL3 platform port, and re-patching every dependency.

Open (Phase 0): does the island's `tpidr_el0` stay free of libnx's TLS? Can the libnx exception handler resume with a
modified context? How big is the heap under hbloader? What are the JIT API's mapping addresses? What happens to threads
left running at exit?

## 2026-11-13: Phase 0 facts from the installed devkit (20261114)

- **The thread pointer is free for the island (checked).** libounce's build rules compile with `-mtp=soft`, so devkit
  code reaches `__thread` variables through `__aarch64_read_tp`, which is `mrs tpidrro_el0; ldr x0, [x0, #0x1f8]`.
  A byte scan for `mrs`/`msr tpidr_el0` finds **0** in libounce.a, libnx.a, libouncevk.a, libouncevk-gl.a, libvulkan.a,
  libEGL/GLES/glapi, libkaihen*.a and kaihen-jit.o. The kernel keeps `tpidr_el0` per thread (libnx's `ThreadContext` has
  a `tpidr` field), so the island can manage `tpidr_el0` as hbxl did. Still to confirm on hardware: two island threads
  keep their own TLS (Phase 1).
- **Exceptions can resume with a modified context (from the disassembly; not yet tried).** The kernel calls
  `__libnx_exception_entry(reason, ctx)`, where ctx holds x0-x8, lr, sp, pc, pstate, afsr0/1, esr and far (x9-x29 are
  still live in registers). libnx copies that into the global `__nx_exceptiondump` and rewrites ctx so that
  `svcReturnFromException(0)` resumes the thread on `__nx_exception_stack` in `__libnx_exception_returnentry`. That
  calls `__libnx_exception_handler(dump)` and then **`svcBreak`**, so stock libnx never resumes the faulting code. It
  also has only one dump and one stack for all threads. But `__libnx_exception_entry` is a **weak** symbol: our own
  entry can rewrite ctx (pc/sp/lr/x0-x8) to enter NativeAOT's fault-to-exception path, then resume. That makes Phase 5
  feasible. While a debugger is attached (svcGetInfo 8), the entry aborts unless `__nx_exception_ignoredebug` is set.
- `vulkan-clear` builds unchanged with the installed devkit (9.5 MB .nro).

## 2026-11-13: lessons from Dusklight's kaiHEN port (`~/switch2hax/dusklight`, `hen/`, CC0)

Dusklight (Twilight Princess decomp) runs on kaiHEN at 60 fps: a C++ app on newlib + libounce, Dawn on OunceVK, SDL3.
Things that matter for osu!:

- **CPU exceptions may not reach the app.** `hen/platform/hen_crash.c`: "Not reached on the HEN release of
  2026-11-09: its loader doesn't pass CPU exceptions on to the app" (tested with and without
  `__nx_exception_ignoredebug`). That puts Phase 5 at risk: re-test with release 20261114c before building on it.
  If exceptions don't arrive, faults stay fatal, as on hbxl.
- **A Switch 2 SDL3 exists and doesn't need any SDL code from us:** devkitPro's SDL `switch-sdl-3.4` rebased on
  `release-3.4.10` plus `hen/patches/sdl3-switch2.patch` (joystick slots, button labels, 1080p handheld, Vulkan
  windows without EGL, pref path). Our `src/native/SDL` is 3.4.10 too. Option for Phase 3: build that SDL3 on the
  **host** side (newlib) and let the island import `SDL_*`, instead of the hbxl SDL's offscreen driver plus the
  `SDL_PushEvent` input shim. The SDL API has no libc types to translate. To be decided once Vulkan works.
- **Threads run on one core** unless each thread widens its own core mask (`hen_thread.c`): Horizon keeps a new
  thread on the process's default core. Our clone emulation must do the same, or the update, draw and audio threads
  share one core. Default pthread stacks are 128 KiB.
- **Exit hygiene** (the loader runs hbmenu in the same process): a Vulkan instance left open means the next app gets
  no GPU (`hen_vulkan.c` destroys leftovers). Threads left waiting on condition variables steal wake-ups from the next
  program (`hen_cond.c`: wake-all plus signalling the waiters at exit; the same as Kaizhar's fix).
- newlib's malloc lock collapses under contention (`hen_malloc_lock.c`); the island uses musl's malloc, so this only
  matters on the host side, and our futex must not hand off through the kernel on every unlock either.
- OunceVK writes its shader cache synchronously, 30-50 ms per write on the SD card (`hen_file_defer.c` queues them on
  a thread). Veldrid's pipelines will hit the same.
- Logs: nxlink stdout, line-buffered, plus `--hen-log-file` on the SD card (`hen_app.c`), with options read from an
  args file when started from hbmenu.
- Licences: Dusklight's port code is CC0, so we may copy from it. Kaizhar is GPLv2+, so read it but don't copy.

## 2026-11-13: CPU exceptions don't reach the app (Test 0b, release 20261114c)

`exctest.nro` (libnx handler + `__nx_exception_ignoredebug = 1`, then a null store) printed its start line over
nxlink and died with no handler output. This matches Dusklight's finding for the 2026-11-09 release.
`ounce-hbloader/switch2/entry.S` does forward `__libnx_exception_entry` to the running NRO. The likely reason it never
runs: hbloader lives inside a game process, and the kernel delivers a user exception to the process's own entry point,
which is the game's startup code. The game's SDK then ends the process. Not verified further.

Consequence: **faults stay fatal, as on hbxl.** NativeAOT can't turn a null dereference into a
NullReferenceException, so Phase 5 is parked. It would need kaiHEN to route exceptions to the loader (a request for
kaiHEN's authors), or a game-specific hook, which isn't worth it. Keep patching null dereferences in osu! one by one;
crash reports must come from logs (nxlink / SD log file), not from a handler.
- Test 0 (vulkan-clear over nxlink) passed. A crash ends the whole game process: relaunch the game with R held; kaiHEN
  stays loaded, so no `hen.cjs` rerun. Every crash costs one game relaunch.

## 2026-11-13: Phase 1 passed: a musl program runs on the lx host (Test 1)

`probes/hello-island` (island: hello.c + lx/island + musl 1.2.6; host: lx/host on libounce) on hardware, first try:
**2022 of 2022 checks**. Four threads kept their own `__thread` values (.tdata and .tbss), which confirms the island
owns `tpidr_el0` next to libnx's `tpidrro_el0`. Also working: stdio over nxlink, malloc (2000 small blocks + 64 MiB),
a 100 ms nanosleep measured as 100 ms, the wall clock, getrandom, sysconf (6 CPUs, core mask 0x3f), a detached thread
unmapping its own stack, pthread_cond_timedwait timing out, and the exit back to the host and hbmenu (228 ms).

- **Memory:** svcGetInfo reports **9152 MiB total** for the process (9149 used, i.e. the heap is set to nearly all of
  it). A 16 GiB malloc (the search's upper limit) succeeded, so heap memory is apparently committed only when it is
  touched. That would make the GC's 1 GiB reservation cheap. Measure real use before relying on it (Phase 2).
- The wall clock is UTC (no time zone yet).
- The final image has no PT_TLS segment (libounce's switch.ld names its PHDRS), but `.tdata`/`.tbss` are 16-aligned
  and musl's TLS offsets match the linker's (16). The island's `.lx_init_array` was empty here; check where it lands
  once C++ constructors are in the island.

## 2026-11-13: Phase 2 passed: .NET NativeAOT runs on kaiHEN (Test 2)

`probes/aot-hello` (ILC object + the stock linux-musl-arm64 NativeAOT libraries, linked into an island by
`tools/aot-island.py`) passed every stage on hardware, the first time: dispatch, GC.Collect, **195 MiB of escaping
allocations in 57 ms with 11 gen0 collections**, 4 threads with [ThreadStatic] and GC while running, throw/catch,
ThreadPool/Task, **a delegate marshalled to native code** (musl's qsort calling a managed comparer through the
prebuilt thunk block), and **System.Console** (which aborted on hbxl for lack of pipe2).

What it took (details in the files):
- `lx/dotnet/lx_thunks.S` + `lx_dotnet.c`: 2040 prebuilt delegate thunks that reach their data with adrp/add, and
  --wraps of RhAllocateThunksMapping / RhpGetThunkDataBlockAddress / RhpGetThunkStubsBlockAddress (from the
  disassembly: managed code converts thunk<->data addresses only through these). pthread_kill suppressed, mlock ok.
- `lx/host/lx_fd.c`: an fd table (console, /dev/null|zero|urandom, SD files and directories with the island's "/" at
  `sdmc:/switch/<app>`, in-process pipes, /proc and /sys = ENOENT), Linux flags and errno.
- `lx/island/island-r.ld`: `__managedcode`/`__modules`/`__unbox` with their `__start_`/`__stop_` symbols (ld only makes
  those in a final link), and **all read-only data moved to `.data.rel.ro`**: musl and every HBLauncher-built library are
  position-dependent, so their constant tables need load-time fixups, which libounce's `-z text` forbids in read-only
  memory.
- `tools/make-island.sh`: drops undefined symbols that no relocation uses (`dst`, `val`: macro debris in the runtime's
  WriteBarriers.S.o), allows musl's `__restore` (sigreturn; never runs). `__dso_handle` is defined by the island.
- The security-cookie fix from HBLauncher is still needed (read-only data can't be made writable).
- Note: .NET 10 stack-allocates small non-escaping arrays; the old stage 3 never touched the GC.

**Clean exit:** after the program returns, the host sets an "exiting" flag, wakes every island thread blocked in the
host (futex waits record their address, sleeps wait on a per-thread word, pipe reads wake every 100 ms), and each one
ends inside the host without returning to island code. A thread still running island code after 2 s would be paused
(`svcSetThreadActivity`). Then all island memory is freed (aot-hello: 300 MiB, mostly the GC's reserved range). Result:
"5 thread(s) ended, 0 paused".

## 2026-11-13: Phase 3 step 1 passed: Vulkan from the island (Test 3)

`probes/vk-island` (vulkan-clear moved into a musl island; every function from vkGetInstanceProcAddr, forwarded to
OunceVK by the host's tracking wrapper `lx/host/lx_vulkan.c`; VK_NN_vi_surface on `lx_vi_window()`): GPU "Switch 2 GPU
(NVK model 0x808)", Vulkan 1.4.0; default window surface **1280x720**, R8G8B8A8_UNORM (37), 3 images, FIFO;
**304 frames in 5.01 s = 60.7 fps**, slowest 22.4 ms; everything destroyed (`0 open`), clean exit, hbmenu fine.
Unlike nn's driver on HBLauncher, nothing special was needed (a UNORM swapchain presents). OunceVK keeps its shader
cache at `sdmc:/config/ouncevk/shader-cache/`. The window size per console mode (Dusklight: 1080p handheld, 4K docked)
is for later.

## 2026-11-13: SDL decision: the HBLauncher-built SDL3 stays inside the island

Option (b), Dusklight's Switch 2 SDL3 in the host, fails on threads: SDL's audio thread and event/hint callbacks would
call managed code from host threads, which have no musl thread pointer and no .NET thread registration. In the island
every SDL thread is an island thread. So: the hbxl SDL3 (offscreen video + hbx drivers, `src/native/SDL` with
`patches/SDL.diff`, unmodified) is linked into the island. Its only non-libc imports are `hbx_input_poll`, `hbxl_ctx`,
`hbxl_sdk_memalign` and `hbxl_sdk_free`, which the island provides. Input (pad, touch, keyboard, mouse, six-axis) and
audio output come from the host through plain C entries in `struct lx_host`, backed by libnx hid and audout. This also
covers the Joy-Con 2 mouse idea.

## 2026-11-13: Phase 3 step 2 passed: Veldrid on OunceVK (Test 4)

`probes/gfx-probe` (ppy.Veldrid, as osu-framework uses it; island built with `aot-island.py --vulkan`, host with
VULKAN=1): every stage. Swapchain 1920x1080 R8G8B8A8_UNORM, clears 16.45 ms/frame, triangle 16.66 ms/frame (60 fps,
FIFO), texture upload/readback, offscreen and textured draws, 192 MiB of ballast textures, and all osu-framework-like
draws (depth LessEqual, UBO set 0 + texture set 1, scissor, alpha blend) with the right pixels. Clip Y inverted, depth
0..1, UBO alignment 64.

- **Swapchain recreation fails on the console's window**, both ways: new swapchain with oldSwapchain then destroying
  the old one (acquire fails), and old destroyed first (VK_ERROR_SURFACE_LOST_KHR). libounce's native_window.h:
  "One producer presents to the layer at a time ... the previous one unregisters the layer's image ... and waits for
  its releases". Veldrid recreated because its 1920x1080 swapchain on the default 1280x720 window presented
  VK_SUBOPTIMAL_KHR. **Fix: the host sizes the window to 1920x1080 before anything is made** (`nwindowSetDimensions`).
  If osu! ever recreates its swapchain, keep the one swapchain (as HBLauncher did).
- None of HBLauncher's nn-driver workarounds were needed (UNORM swapchain presents correctly, no gamma issue).
- `lx/dotnet/lx_vk.c` keeps the lazy trampolines (785 commands) and the Wayland->vi translation, and logs failed
  acquires/presents. The island's fake libdl is general now (Vulkan; OpenSSL and FFmpeg tables via weak symbols).
- Teardown printed `MESA: warning: GPU retirement after the last instance: status 0x104`; everything was destroyed.
  Watch whether hbmenu ever lacks its GPU after a run.
- For later (libounce native_window.h): `nwindowSetModeDimensions` (per-mode size, e.g. 4K docked) and
  `nwindowSetFrameRates` (120 Hz on the handheld screen, VRR): osu! at 120 fps handheld.

## 2026-11-13: Phase 3 step 3 passed: networking (Test 5)

`probes/net-probe` on hardware: **13 of 13** after one fix. DNS through the console's resolver (osu.ppy.sh -> 2
addresses, 74 ms), blocking and async TCP (epoll, 53 MiB/s), HTTP, HTTPS with a chain build against the probe CA,
10 concurrent HTTPS requests, 20 MiB HTTP to the SD card (10.4 MiB/s, the card is the limit) and 4 MiB HTTPS
(50 MiB/s), WebSocket echo, cancellation, connection refused, 40 sockets at once, and one HTTPS GET to osu.ppy.sh
verified against the CA bundle.

- `lx/host/lx_net.c`: libnx BSD sockets translated to Linux (sockaddr without sin_len, SOL_SOCKET 1, SO_/MSG_
  values, errno); every host socket non-blocking with blocking calls and SO_RCVTIMEO/SNDTIMEO emulated by 10 ms polls;
  epoll emulated with armed bits (any receive/send re-arms); DNS via newlib's getaddrinfo (sfdnsres). Island side:
  `lx/island/lx_net.c` (getaddrinfo/freeaddrinfo wraps, from hbxl_net.c). ABI version 3 (`resolve_ipv4`).
- **The one bug:** closing an fd must drop it from epoll sets (Linux does). .NET reuses fd numbers at once, so the next
  socket's EPOLL_CTL_ADD hit the stale entry: EEXIST, which surfaced as "Unknown socket error" on connect. Entries
  now remember the open file's generation number (not its address: malloc reuses those too).
- romfs: the host mounts the .nro's romfs; read-only lookups that miss on the SD card fall back to it (the probe carries
  its CA and the CA bundle that way). `ROMFS=dir` in the host Makefile.
- make-island: weak references are resolved inside the island (relink with -u; still missing -> 0). OpenSSL's weak
  getentropy would otherwise have bound to newlib's. The svc check reads the object directly (`tools/svc-scan.py`);
  objdump -d took over 8 minutes on an island with OpenSSL.

## 2026-11-14: osu! starts on kaiHEN (Test 6, runs 1-4)

osu-game links as an island (168 MB .nro) and runs: LinuxGameHost, SDL3 (offscreen), the Vulkan renderer on OunceVK
(1920x1080 swapchain), **draw and update at 60 fps**, GC quiet (16 MiB heap), delegate thunks in use. The screen stayed
empty and osu! never got past its database. What the runs found, in order:

1. **Realm's lock file** (`client.realm.lock`): Realm sizes it with posix_fallocate before mapping it. The host answered
   fallocate with success without growing the file -> Realm saw a 0-byte lock file and retried forever (25 000 log
   lines). Fixed: `lx_fallocate` really extends the file (or the shared buffer).
2. **The console's file system refuses to open a file again while it is open for writing (EIO).** Linux allows it. It
   hit the shm write-back (second handle), the shm read-in (Realm saw an empty lock file again) and finally Realm itself
   (it opens the lock file twice: "Failed to open file ... client.realm.lock: I/O error", then Realm started a fresh
   database). Fixed (built, NOT yet run on hardware): **one shared card handle per file** (`struct hfile` in lx_fd.c),
   reference-counted, every island descriptor with its own position, positioned transfers under the handle's lock, a
   read-only handle reopened read-write when a writer joins. Shared mappings write back through it.
3. **osu-framework recreates its swapchain once at start** ("vkCreateSwapchainKHR ... replacing one"). In gfx-probe a
   second swapchain on the console's window stopped presenting. This is the prime suspect for the empty screen even
   at 60 fps: next step is to keep the one swapchain in lx_vk.c (HBLauncher did) when a recreation has the same size.
4. Build-side findings: COMDAT groups must be dissolved in the island link (`--force-group-allocation`: the final link
   kept OunceVK's copy of C++ typeinfo and dropped the island's); osu!'s relocatable object has >65535 sections (ELF
   then keeps the counts in section header 0: tools/svc-scan.py and tools/unused-undef.py handle it; `readelf -rW`
   took 16+ minutes on it and is no longer used); FFmpeg must be position-independent (its NEON assembly has absolute
   addresses in .text otherwise: tools/build-ffmpeg.sh, output src/native/ffmpeg/build-lx); OpenSSL's weak getentropy
   and libstdc++'s weak `_ITM_*` are resolved inside the island.
5. Harmless: MIDI's ALSA lookup fails and is caught; inotify (EMFILE now), sched_getparam/getscheduler answered.
6. ounceshell's FTP server (port 1337) crashed on `LIST /` (a path argument). `tools/ftp-upload.py` uses only CWD by
   bare name, CDUP, plain LIST, MKD and STOR; it put osu-resources (127 MiB) on the card at 1.4 MiB/s.

## 2026-11-14: osu! renders and takes input (Test 6, runs 5-10)

- **Run 5:** the shared card handles work: Realm opens ("Opened realm ... at version 52"), osu! reaches the main menu
  at 60 fps. It died after ~20 s with the system's "there was a problem and the software needed to close" (a CPU
  fault; Test 0b: faults never reach the app).
- **Runs 6-8:** the same fault after ~3 s, right after "switch2 settings: loaded". The syscall trace of runs 5 and 8
  ended the same way on one thread: `openat(".../cache/fonts", O_DIRECTORY)`, fcntl, mmap: the start of a directory
  listing. **Cause: lx_getdents64 used newlib's telldir/seekdir to push back an entry that didn't fit the 2 KiB
  buffer; they don't work on libnx's DIR.** Only a listing over one buffer takes that path, and the font cache
  (~75-character glyph-page names) was the first, once a previous run had filled it. Fixed with a one-entry stash in
  the fd. Lesson: when every run dies at the same syscall, suspect the host's handler of the NEXT call too.
- **Empty screen:** osu-framework recreates its swapchain once at start (and again for a present mode change, FIFO ->
  MAILBOX). lx_vk.c now hands back the existing swapchain for a same-size recreation and swallows the destroy of the
  "old" one (HBLauncher did the same), and gives back the image acquired before the reuse on the next acquire (empty
  submit signalling its semaphore/fence), as HBLauncher's shim did. `LX_VK_KEEP_SWAPCHAIN=0` turns it off.
- **Run 9:** first-run setup on screen (ShadowCast), main menu at 60 fps draw / 240 Hz update, stable for 75 s.
- The card's file system also fails **stat (ENOENT) and unlink (EIO) on an open file**: osu!'s online.db.bz2 download
  (three concurrent writers, then open-for-read -> ENOENT, delete -> EIO). stat/open now look up the shared handle
  first, and unlink of an open file is deferred to its last close (the path reads as gone meanwhile).
- **Run 10: input works, all of it** (user-tested): controllers (buttons ORed, each stick from the pad pushed
  furthest), touch screen, gyro cursor (4 six-axis sensors started: handheld, full key, Joy-Con pair), USB keyboard
  and mouse, and the **Joy-Con 2 mouse** through libounce's PadPointer (absolute 1920x1080 pointer; the Joy-Con's
  L/ZL or R/ZR click, so those buttons send no key while it is active). Host: `lx/host/lx_input.c`, one
  `input_poll(struct lx_input *, flags)` entry (ABI 4). Island: `console/osu-game/shim/lx_input.c` wraps
  SDL_PumpEvents and pushes SDL events (the HBLauncher mapping, minus its SDK offsets and watchdog).
- The first ShadowCast frame after opening the device can be black even with `select=gte(n,10)`: take a second one.

## 2026-11-14: audio, GC downgrade, clean exit (runs 11-14)

- **Audio (run 11, user-tested: no crackles, almost no latency):** ABI 5 adds audio_open/submit/queued/close
  (`lx/host/lx_audio.c`, plain libnx audout: works on kaiHEN as in Kaizhar). `shim/lx_audio_out.c` takes over
  mysoundlib's device stream like hbxl_audio_out.c did; default 3 x 128 frames (8 ms), the Switch 2 settings set it
  (the user's saved 2 x 128 = 5.3 ms also played clean). The dropout counter counted sub-millisecond "gaps" ~25 times
  a second (the estimate counts the playing buffer whole): it now ignores gaps under 1 ms.
- **GC:** `shim/lx_gc.c`: AddMemoryPressure's gen2 runs as gen1 ("induced gen 1" in the log). Pauses 2-30 ms with
  suspension in microseconds (hbxl: 25-35 ms typical, pressure gen2s up to 312 ms).
- **Exit (runs 12-14):** osu!'s teardown and the island's exit were already clean; the host died in its Vulkan
  leftover cleanup. osu-framework leaves its device, swapchain, surface and instance open, and (1) the host destroyed
  the device under a live swapchain, (2) the host's real destroy functions were NULL: they are learned when the island
  first looks them up, and osu! never destroys its device. Now swapchains -> device -> surfaces -> instance, with
  missing functions resolved at exit. The host also closes audout and releases the Joy-Con 2 mouse and six-axis
  sensors. **hbmenu comes back and launches the next program** (run 14). Still printed: `MESA: warning: GPU
  retirement after the last instance: status 0x102` (harmless so far).
- Undocking drops the ethernet and with it the nxlink log; the game keeps running.
