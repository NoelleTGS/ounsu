# The musl toolchain (`toolchain/`)

Everything in the island (the NativeAOT output, the native libraries, musl libc) is built with an `aarch64-linux-musl`
cross toolchain: GCC 15.1.0, binutils 2.44 and musl 1.2.6 from [musl-cross-make](https://github.com/richfelker/musl-cross-make)
(`5e8042e63de29f8473fc3b0b831a587ba284e1a8`), with one extra musl patch.

## The patch: `musl-hbx-syscall-hooks.diff`

From RavensClawX's [HBLauncher](https://github.com/ravensclawx/HBLauncher) (`sdk/linux/musl-hbx-syscall-hooks.diff`,
MIT; the notice is in `LICENSE`), unchanged. Every syscall musl makes calls `__hbx_syscall(n, a, b, c, d, e, f)` instead
of issuing `svc 0`, and the assembly that would issue `svc` itself (`__clone`, `__syscall_cp_asm`, `__unmapself`,
`vfork`) goes through hooks too. A raw Linux `svc 0` would be a Horizon SVC on the console. `lx/island/lx_island.c`
defines `__hbx_syscall` and hands each call to the host; `tools/make-island.sh` checks that no `svc` is left in the island.

## Building it

Needs a normal native GCC, make, and about 3 GB of disk space during the build.

```sh
git clone https://github.com/richfelker/musl-cross-make toolchain-src
git -C toolchain-src checkout 5e8042e63de29f8473fc3b0b831a587ba284e1a8
mkdir -p toolchain-src/patches/musl-1.2.6
cp patches/musl-hbx-syscall-hooks.diff toolchain-src/patches/musl-1.2.6/9001-hbx-syscall-hooks.diff
cat > toolchain-src/config.mak <<'EOF'
TARGET = aarch64-linux-musl
OUTPUT = $(CURDIR)/toolchain
GCC_VER = 15.1.0
MUSL_VER = 1.2.6
BINUTILS_VER = 2.44
COMMON_CONFIG += CFLAGS="-g0 -O2" CXXFLAGS="-g0 -O2 -fno-char8_t" LDFLAGS="-s"
# The console's code pages are BTI-guarded: every target library gets landing pads.
GCC_CONFIG += --enable-languages=c,c++ --disable-multilib --disable-libsanitizer --enable-default-pie=no --disable-libquadmath
GCC_CONFIG += CFLAGS_FOR_TARGET="-O2 -g0 -mbranch-protection=bti" CXXFLAGS_FOR_TARGET="-O2 -g0 -mbranch-protection=bti"
MUSL_CONFIG += CFLAGS="-O2 -g0 -mbranch-protection=bti"
EOF
make -C toolchain-src -j"$(nproc)" && make -C toolchain-src install
ln -s toolchain-src/toolchain toolchain
```

The build scripts look for the toolchain in `toolchain/` at the repository's root (`LX_MUSL_TOOLCHAIN` overrides it).
`tools/gen-notices.py` takes musl's `COPYRIGHT` from the musl sources next to it (`toolchain-src/musl-1.2.6`; `MUSL_SRC`
overrides it). Neither folder is tracked.
