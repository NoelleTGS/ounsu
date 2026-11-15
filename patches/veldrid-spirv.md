# veldrid-spirv (`src/native/veldrid-spirv`)

Upstream: [ppy/veldrid-spirv](https://github.com/ppy/veldrid-spirv) `2026.1106.0` (`veldrid-spirv.base`), with
SPIRV-Cross, shaderc, glslang, SPIRV-Tools and friends under `ext/`. Patch: `veldrid-spirv.diff`.

osu-framework's Veldrid renderer compiles its GLSL shaders to SPIR-V at runtime (then caches them) through this native
library. Normally it's a shared library from NuGet; the console needs it static.

## Changes

| File | Change |
|---|---|
| `CMakeLists.txt` | `LIBRARY_TYPE STATIC` |
| `ext/*/CMakeLists.txt` | `cmake_minimum_required` raised to 3.16 (current CMake refuses the old minimums) |
| `src/libveldrid-spirv/libveldrid-spirv.cpp` | `#include <cstdint>` (GCC 15 no longer includes it transitively) |

Linking the real `-lstdc++` it needs means dropping NativeAOT's `libstdc++compat.a` (`--exclude libstdc++compat.a` on
`tools/aot-island.py` in `console/osu-game/build.sh`). `sh tools/build-native.sh veldrid-spirv` builds it with the musl toolchain into `build-lx/`.

## Restoring

```sh
git -C src/native/veldrid-spirv checkout -f 039422c80c113623afd70146f2015e7824130bd6
git -C src/native/veldrid-spirv submodule update --init --recursive
git -C src/native/veldrid-spirv apply --whitespace=nowarn patches/veldrid-spirv.diff
sh tools/build-native.sh veldrid-spirv
```
