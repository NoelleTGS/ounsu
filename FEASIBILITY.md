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
