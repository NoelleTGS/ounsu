# SDL (`src/native/SDL`)

Upstream: [libsdl-org/SDL](https://github.com/libsdl-org/SDL) 3.4.x, base in `SDL.base` (`8e37db5`), **unmodified**
(`SDL.diff` is empty).

**SDL's own `CLAUDE.md` forbids AI-generated code in SDL.** Anything the port needs from SDL goes into the port's own
shims instead (`--wrap` plus SDL's public API, in `console/osu-game/shim/`):

- Video: SDL's stock offscreen driver. The display goes through Vulkan on the console's window (see `osu-framework.md`).
- Audio: osu!'s mixer output is taken from SDL's audio stream calls (`--wrap=SDL_OpenAudioDeviceStream` and friends) by
  `shim/lx_audio_out.c` and played through the host's audout (`lx/host/lx_audio.c`). SDL itself runs its dummy driver
  (`SDL_AUDIO_DRIVER=dummy`).
- Input: `shim/lx_input.c` wraps `SDL_PumpEvents`, reads the controllers, touchscreen, keyboard and mouse through the
  host (`lx/host/lx_input.c`) and pushes ordinary SDL events.

`sh tools/build-native.sh sdl` builds it with the musl toolchain as a static library (`SDL_UNIX_CONSOLE_BUILD`: no X11
or Wayland) into `build-lx/libSDL3.a`.

## Restoring

```sh
git -C src/native/SDL checkout -f 8e37db5e797b6167f3a00d697d816a684bd259c7
sh tools/build-native.sh sdl
```
