# Hardware tests for the osu! kaiHEN port

The user runs every test on the console; Claude reads the logs on the PC. The hbxl-era tests (BTI probe, offline play,
SmileBASIC sessions, ...) are in the separate osu-HBLauncher repository.

## Once per console boot: start kaiHEN

1. On the console: SmileBASIC in Direct mode.
2. On the PC: `cd ~/switch2hax/ns2jb && node hen.cjs --dir ~/switch2hax/kaiHEN-20261114c 192.168.1.206`, then follow
   the printed SmileBASIC steps until `HEN launch completed!`. Close SmileBASIC normally.
   The exploit succeeds about 20% of the time; after a failure, fully reboot the console.
3. Hold **R** while starting a Switch 2 game from internal storage. hbmenu should appear. On a fresh setup the first
   launch only captures the game's policy: close it, then launch again with R held.

## Development loop (to be confirmed in Phase 0)

- hbmenu's netloader: press Y in hbmenu, then on the PC `nxlink -s -a 192.168.1.177 <app>.nro`. `-s` keeps the
  connection open, and the program's stdout/stderr arrive on the PC. Save it with `| tee logs/<name>.log` so
  Claude can read it.
- Without the PC: copy the `.nro` to `sdmc:/switch/` and start it from hbmenu.

## Test 0: vulkan-clear (devkit sanity check)

Build `~/switch2hax/ounce-devkit/examples/vulkan-clear`, send it with nxlink, and check that the screen is cleared and
that it returns to hbmenu. **Passed 2026-11-13:** the screen cycles through colours (that's what the example draws);
Plus returns to hbmenu. The nxlink loop works (`-s` relays stdout).

## Test 0b: do CPU exceptions reach the app?

A tiny program that installs libnx's exception handler and then dereferences null (source:
`/tmp/claude-1000/exctest/source/main.c`, to move into `probes/` once the island exists). Send it with
`nxlink -s -a 192.168.1.177 /tmp/claude-1000/exctest/exctest.nro`. `exctest: HANDLER REACHED` in the output means the
loader passes exceptions on, and faults can become managed exceptions (Phase 5). If the app dies after the `start`
line with nothing more, they don't (Dusklight saw that with the 2026-11-09 release). Afterwards, check that hbmenu
still works or whether the console needs a reboot.
**Result 2026-11-13: not reached.** The whole game process ends, with no handler output. Relaunch the game with R held
to get hbmenu back (kaiHEN stays loaded, so no `hen.cjs` rerun is needed).

## Test 1: hello-island (Phase 1: a musl program on the lx host)

Build: `sh probes/hello-island/build.sh` → `probes/hello-island/build/hello-island.nro`. Send it with
`nxlink -s -a 192.168.1.177 probes/hello-island/build/hello-island.nro`.

Expected output: `lx: host up`, the process memory and largest malloc (Phase 0's heap question), the TLS layout, then
the program's `hello:` lines: environment, malloc, a 100 ms sleep, wall clock, random bytes, CPU count, four workers
with `thread-local values ok`, and `hello: done, N of N checks passed`. It ends with
`lx: program exited with status 0` and `lx: back to hbmenu`, and hbmenu should come back by itself. **Passed 2026-11-13** (2022/2022 checks).

## Test 2: aot-hello (Phase 2: .NET NativeAOT on the lx host)

Build: `sh probes/aot-hello/build.sh` → `probes/aot-hello/build/aot-hello.nro`. Send it with
`nxlink -s -a 192.168.1.177 probes/aot-hello/build/aot-hello.nro`.

Each stage logs `aot-hello: stage N: ...` before it runs and `stage N: ok` after it: 0 managed Main, 1 virtual and
interface dispatch, 2 GC.Collect, 3 ~200 MiB of allocation churn, 4 threads + [ThreadStatic] + GC while they run,
5 throw/catch, 6 ThreadPool + Task, 7 System.Console. Success ends with `RESULT: all stages passed` and
`lx: program exited with status 0`. If it dies, the last `stage` line says where; `lx: syscall N not implemented`
lines show what the host is missing. **Passed 2026-11-13** (all stages, plus 6b: a delegate marshalled to native code; clean exit).
