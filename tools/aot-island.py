#!/usr/bin/env python3
"""Turn a captured NativeAOT link line into an island object (for the lx host).

    python3 tools/aot-island.py CAPTURE PROJECT_DIR OUT.o [EXTRA_INPUT...] [--exclude NAME]...

CAPTURE is the argument list probes/aot-hello/tools/capture-link.sh recorded (one per line); relative paths in it are
relative to PROJECT_DIR (where dotnet publish ran). Keeps the ILC object and the runtime's archives, drops the
clang/glibc flags, adds lx/dotnet (delegate thunks, pthread_kill, mlock) with its --wrap flags, and runs
tools/make-island.sh. EXTRA_INPUTs (objects, archives, C/asm sources, linker flags) go after the captured inputs;
.c/.S sources are compiled with the musl toolchain first. --exclude drops a captured input by file name
(e.g. libstdc++compat.a when the app links the real libstdc++).

The runtime archive libRuntime.*.a is copied with __security_cookie moved from .rodata to writable data: start-up
mprotects the cookie's page writable to store a random value, and the .nro's read-only data can't be made writable
(mprotect is a no-op here). This is the HBLauncher fix, unchanged.
"""
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TC = Path(os.environ.get("LX_MUSL_TOOLCHAIN", Path.home() / "osu-HBLauncher/toolchain-src/toolchain")) / "bin"
TOOL = lambda name: str(TC / f"aarch64-linux-musl-{name}")

WRAPS = ["RhAllocateThunksMapping", "RhpGetThunkDataBlockAddress", "RhpGetThunkStubsBlockAddress"]
CFLAGS = ["-O2", "-g", "-Wall", "-Wextra", "-Werror", "-fPIE", "-ffunction-sections", "-fdata-sections",
          f"-I{ROOT / 'lx/include'}"]


def writable_cookie(archive: Path, work: Path) -> Path:
    out = work / (archive.stem + ".lx.a")
    shutil.copyfile(archive, out)
    tmp = work / "cookie"
    tmp.mkdir(exist_ok=True)
    subprocess.run([TOOL("ar"), "x", str(archive), "startup.cpp.o"], cwd=tmp, check=True)
    obj = tmp / "startup.cpp.o"
    sections = subprocess.run([TOOL("readelf"), "-SW", str(obj)], capture_output=True, text=True, check=True).stdout
    rodata = [l for l in sections.splitlines() if " .rodata " in l]
    if len(rodata) != 1 or " 000008 " not in rodata[0]:
        sys.exit(f"aot-island: unexpected .rodata in {archive.name} startup.cpp.o:\n{rodata}")
    subprocess.run([TOOL("objcopy"), "--rename-section", ".rodata=.data.lx_cookie,alloc,load,contents,data",
                    str(obj)], check=True)
    subprocess.run([TOOL("ar"), "r", str(out.resolve()), "startup.cpp.o"], cwd=tmp, check=True)
    return out


def compile_source(src: Path, work: Path) -> Path:
    out = work / (src.name + ".o")
    flags = CFLAGS if src.suffix == ".c" else ["-g"]
    subprocess.run([TOOL("gcc"), *flags, "-c", str(src), "-o", str(out)], check=True)
    return out


def main() -> None:
    args = sys.argv[1:]
    excludes = set()
    while "--exclude" in args:
        i = args.index("--exclude")
        excludes.add(args[i + 1])
        del args[i:i + 2]
    if len(args) < 3:
        sys.exit(__doc__)
    capture, project, out = Path(args[0]), Path(args[1]), Path(args[2])
    extras = args[3:]

    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        inputs = []
        for a in capture.read_text().splitlines():
            if a.startswith("-"):
                continue
            p = Path(a)
            if p.suffix not in (".o", ".a"):
                continue
            if not p.is_absolute():
                p = project / p
            if p.name in excludes:
                continue
            if p.name.startswith("libRuntime.") and p.suffix == ".a":
                p = writable_cookie(p, work)
            inputs.append(str(p))
        if not any(i.endswith(".o") and "/native/" in i and "libbootstrapper" not in i for i in inputs):
            sys.exit("aot-island: no ILC object in the captured link line")

        runtime = [compile_source(ROOT / "lx/dotnet/lx_dotnet.c", work),
                   compile_source(ROOT / "lx/dotnet/lx_thunks.S", work)]
        extra_inputs = []
        for e in extras:
            p = Path(e)
            extra_inputs.append(str(compile_source(p, work)) if p.suffix in (".c", ".S") else e)

        flags = [f"-Wl,--wrap={w}" for w in WRAPS] + ["-Wl,--defsym,__xmknod=mknod"]
        subprocess.run(["sh", str(ROOT / "tools/make-island.sh"), str(out), *flags,
                        *map(str, runtime), *inputs, *extra_inputs], check=True)


if __name__ == "__main__":
    main()
