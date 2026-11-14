# SDL (`src/native/SDL`)

Upstream: [libsdl-org/SDL](https://github.com/libsdl-org/SDL) 3.4.x, base in `SDL.base` (`8e37db5`). Patch: `SDL.diff`.

**SDL's own `CLAUDE.md` forbids AI-generated code in SDL.** This patch is not this port's work: it's the `hbx` platform
patch from RavensClawX's [Twilight Princess HBLauncher port](https://github.com/ravensclawx/TwilightPrincess-HBLauncher),
applied unchanged. Anything the osu! port needs from SDL goes into the port's own shims instead (`--wrap` plus SDL's
public API, in `console/osu-game/shim/`).

**On kaiHEN the hbx drivers are not used.** They read HBLauncher's context (`hbxl_ctx`), which
`console/osu-game/shim/lx_osu.c` sets to NULL, so both decline and SDL falls back to other drivers. The patched SDL
build is simply the one reused from the HBLauncher version (it is a musl build, like the rest of the island):

- Audio: osu!'s mixer output is taken from SDL's audio stream calls (`--wrap=SDL_OpenAudioDeviceStream` and friends) by
  `shim/lx_audio_out.c` and played through the host's audout (`lx/host/lx_audio.c`). SDL itself runs the dummy
  driver (`SDL_AUDIO_DRIVER=dummy`).
- Input: `shim/lx_input.c` wraps `SDL_PumpEvents`, reads the controllers, touchscreen, keyboard and mouse through the
  host (`lx/host/lx_input.c`) and pushes ordinary SDL events.
- Video: SDL's stock offscreen driver; the display goes through Vulkan (see `osu-framework.md`).

## What the hbx patch adds (HBLauncher only)

| File | What |
|---|---|
| `src/audio/hbx/SDL_hbxaudio.c` | Audio driver on the console's AudioOut (4 × 1024-frame buffers, ~85 ms). In the HBLauncher version, osu! bypassed it with its own lower-latency output |
| `src/joystick/hbx/SDL_hbxjoystick.c` | Joystick driver on HBLauncher's pad input (SDK sticks are -100..100 with +y up; SDL wants +y down). osu!'s menus ignore joysticks, so the input shim turns pads into mouse/keyboard events instead |
| `src/audio/SDL_audio.c`, `SDL_sysaudio.h`, `src/joystick/SDL_joystick.c`, `SDL_sysjoystick.h`, `CMakeLists.txt` | Registers both drivers |

Built with the musl toolchain as a static library into `build-hbx/` (`libSDL3.a`).

## Restoring

```sh
git -C src/native/SDL checkout -f 8e37db5e797b6167f3a00d697d816a684bd259c7
git -C src/native/SDL apply --whitespace=nowarn patches/SDL.diff
```
