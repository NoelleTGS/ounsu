# Hardware tests

Every test runs on the console. `<console IP>` is the Switch 2's address (ounceshell shows it), `<PC IP>` the PC's.

## Once per console boot: start kaiHEN

1. On the console: SmileBASIC in Direct mode.
2. On the PC, in an ns2jb checkout: `node hen.cjs --dir <kaiHEN release folder> <PC IP>`, then follow the printed
   SmileBASIC steps until `HEN launch completed!`. Close SmileBASIC normally.
   The exploit succeeds about 20% of the time; after a failure, fully reboot the console.
3. Hold **R** while starting a Switch 2 game from internal storage. hbmenu should appear. On a fresh setup the first
   launch only captures the game's policy: close it, then launch again with R held.

## Development loop

- hbmenu's netloader: press Y in hbmenu, then on the PC `nxlink -s -a <console IP> <app>.nro > logs/<name>.log 2>&1`.
  `-s` keeps the connection open, and the program's stdout/stderr arrive on the PC. nxlink prints "exiting ..." when
  the connection drops (crash, exit, or undocking, which cuts a wired connection); a crash shows no
  `lx: program exited` line.
- Without the PC: copy the `.nro` to `sdmc:/switch/` and start it from hbmenu. `LX_LOG_FILE=/lx-last.log` in
  `/switch/<app>/lx-env` keeps the log on the card.
- `LX_TRACE=1` in lx-env (or `LX_ENV_EXTRA="LX_TRACE=1"` on `console/osu-game/build.sh`) logs every syscall.

## Test 0: vulkan-clear (devkit sanity check)

Build the ounce-devkit's `examples/vulkan-clear`, send it with nxlink, and check that the screen is cleared and that it
returns to hbmenu. **Passed 2026-11-13:** the screen cycles through colours; Plus returns to hbmenu.

## Test 0b: do CPU exceptions reach the app?

A small program that installs libnx's exception handler and then dereferences null. **Result 2026-11-13: not
reached.** The whole game process ends with no handler output, so CPU faults can't become managed exceptions (see
FEASIBILITY.md). Relaunching the game with R held brings hbmenu back (kaiHEN stays loaded).

## Test 1: hello-island (a musl program on the lx host)

Build: `sh probes/hello-island/build.sh`. Send `probes/hello-island/build/hello-island.nro` with nxlink.

Expected output: `lx: host up`, the process memory and largest malloc, the TLS layout, then the program's `hello:`
lines: environment, malloc, a 100 ms sleep, wall clock, random bytes, CPU count, four workers with
`thread-local values ok`, and `hello: done, N of N checks passed`. It ends with `lx: program exited with status 0` and
`lx: back to hbmenu`, and hbmenu comes back by itself. **Passed 2026-11-13** (2022/2022 checks).

## Test 2: aot-hello (.NET NativeAOT on the lx host)

Build: `sh probes/aot-hello/build.sh`. Send `probes/aot-hello/build/aot-hello.nro` with nxlink.

Each stage logs `aot-hello: stage N: ...` before it runs and `stage N: ok` after it: 0 managed Main, 1 virtual and
interface dispatch, 2 GC.Collect, 3 ~200 MiB of allocation churn, 4 threads + [ThreadStatic] + GC while they run,
5 throw/catch, 6 ThreadPool + Task, 6b a delegate marshalled to native code, 7 System.Console. Success ends with
`RESULT: all stages passed` and `lx: program exited with status 0`. If it dies, the last `stage` line says where;
`lx: syscall N not implemented` lines show what the host is missing. **Passed 2026-11-13.**

## Test 3: vk-island (Vulkan from the island)

Build: `sh probes/vk-island/build.sh`. Send `probes/vk-island/build/vk-island.nro` with nxlink.

Expected: `lx-vk: vkCreateInstance (1 open)`, the GPU name and Vulkan version, the surface size, a swapchain line,
then the screen cycles through colours for 5 seconds and the program reports frames/fps (about 60) and its slowest
frame. Afterwards every `lx-vk:` destroy line, `0 open`, and hbmenu draws normally. **Passed 2026-11-13.**

## Test 4: gfx-probe (Veldrid on OunceVK)

Build: `sh probes/gfx-probe/build.sh` (needs `glslc`). Send `probes/gfx-probe/build/gfx-probe.nro` with nxlink.

Stages: 0 managed Main, 1 the vi window, 2 Veldrid GraphicsDevice.CreateVulkan (Wayland source mapped to
VK_NN_vi_surface; swapchain size and format), 3 120 frames of clears (ms/frame), 4 buffers + SPIR-V shaders + a
pipeline, 5 300 frames of a spinning triangle (visible on screen), 7a-7g GPU readback (texture upload, offscreen
draws, 192 MiB of ballast textures), 8a-8g osu-framework-like draws (depth, UBOs, texture sets, scissor, blending:
each line says ok or the wrong pixel), 6 dispose. Ends with `RESULT: all stages passed` and the host's Vulkan objects
back at `0 open`. **Passed 2026-11-13** (60 fps).

## Test 5: net-probe (sockets, epoll, DNS, TLS)

On the PC: `python3 tools/net-probe-server.py --host <PC IP>` (creates the test CA on first start; keep it running).
Build: `sh probes/net-probe/build.sh` (the CA, the CA bundle and the server's address go into the .nro's romfs:
nothing to copy to the SD card). Send `probes/net-probe/build/net-probe.nro` with nxlink.

Stages: 0 CSPRNG + clock, 1 blocking TCP, 2 DNS (console resolver), 3 async socket 4 MiB (epoll), 4 HTTP,
5 HTTPS with chain build, 6 10 concurrent HTTPS, 7 downloads (20 MiB HTTP to the SD card, 4 MiB HTTPS),
8 WebSocket echo, 9 timeout/cancellation, 10 connection refused, 11 40 sockets at once, 12 one HTTPS GET to osu.ppy.sh
(the only request to ppy's servers). Ends with `done: N passed, M failed`. **Passed 2026-11-13** (13/13).

## osu! itself

Build: `sh console/osu-game/build.sh`. Needs `/switch/ounsu/osu-resources` on the SD card (README, "From source").
Send `console/osu-game/build/ounsu.nro` with nxlink and go through the checklist in `UPDATING.md`, step 6: start-up
and main menu, song select and a map in each ruleset, audio, skins, settings (including "Switch 2"), online features,
importing, docking/undocking, Joy-Con mouse, touch, and the exit back to hbmenu. The log should show no new
`exception`/`error`/`fail` lines.
