# SDL (`src/native/SDL`)

Upstream: [libsdl-org/SDL](https://github.com/libsdl-org/SDL) 3.4.x, base in `SDL.base` (`8e37db5`). Patch: `SDL.diff`.

**SDL's own `CLAUDE.md` forbids AI-generated code in SDL.** This patch is not this port's work: it's the `hbx` platform
patch from RavensClawX's [Twilight Princess HBLauncher port](https://github.com/ravensclawx/TwilightPrincess-HBLauncher),
applied unchanged. Anything the osu! port needs from SDL goes into the port's own shims instead (`--wrap` plus SDL's
public API, in `console/osu-game/shim/`).

## What the hbx patch adds

| File | What |
|---|---|
| `src/audio/hbx/SDL_hbxaudio.c` | Audio driver on the console's AudioOut (4 × 1024-frame buffers, ~85 ms). osu! normally bypasses it with its own lower-latency output (`shim/hbxl_audio_out.c`); `HBXL_AUDIO_SDL=1` uses it |
| `src/joystick/hbx/SDL_hbxjoystick.c` | Joystick driver on HBLauncher's pad input (SDK sticks are -100..100 with +y up; SDL wants +y down). osu!'s menus ignore joysticks, so `shim/hbxl_input.c` turns pads into mouse/keyboard events |
| `src/audio/SDL_audio.c`, `SDL_sysaudio.h`, `src/joystick/SDL_joystick.c`, `SDL_sysjoystick.h`, `CMakeLists.txt` | Registers both drivers |

Video uses SDL's stock offscreen driver; the console display is driven through Vulkan (see `osu-framework.md`).

Built as a static library into `build-hbx/` (`libSDL3.a`).

## Restoring

```sh
git -C src/native/SDL checkout -f 8e37db5e797b6167f3a00d697d816a684bd259c7
git -C src/native/SDL apply --whitespace=nowarn ~/osu-HBLauncher/patches/SDL.diff
```
