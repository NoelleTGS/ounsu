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
