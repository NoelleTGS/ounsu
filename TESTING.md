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
that it returns to hbmenu.
