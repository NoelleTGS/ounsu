#!/usr/bin/env python3
"""Turn the captured NativeAOT link line into an hbxl-build app manifest.

Keeps the ILC object and the NativeAOT runtime archives; drops clang/glibc-style
flags (-static-pie, version scripts, -ldl ...) that hbxl's linker script replaces.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def fs_host_flags():
    """HBX_FS_HOST=a.b.c.d: the PC running hbxl-fileserver (default 10.42.0.1)."""
    host = os.environ.get("HBX_FS_HOST")
    if not host:
        return []
    a, b, c, d = (int(x) for x in host.split("."))
    return [f"-Wl,--defsym=HBXL_FS_HOST=0x{a | b << 8 | c << 16 | d << 24:08x}"]

def writable_cookie(archive, out_dir):
    """Copy a NativeAOT runtime archive with __security_cookie made writable.

    startup.cpp.o keeps the cookie alone in an 8-byte .rodata section; startup
    mprotects that page RW, writes a random value and restores it.  hbxl's
    mprotect is a no-op and the code image is mapped RX, so the write faults.
    Renaming the section to writable data puts the cookie in the data region.
    """
    tc = Path(os.environ.get("HBXL_TOOLCHAIN", "")) / "bin"
    out = out_dir / (archive.stem + ".hbxl.a")
    shutil.copyfile(archive, out)
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run([tc / "aarch64-linux-musl-ar", "x", archive, "startup.cpp.o"], cwd=tmp, check=True)
        obj = Path(tmp) / "startup.cpp.o"
        sections = subprocess.run([tc / "aarch64-linux-musl-readelf", "-SW", obj],
                                  capture_output=True, text=True, check=True).stdout
        rodata = [l for l in sections.splitlines() if " .rodata " in l]
        if len(rodata) != 1 or " 000008 " not in rodata[0]:
            sys.exit(f"make-manifest: unexpected .rodata in {archive.name} startup.cpp.o:\n{rodata}")
        subprocess.run([tc / "aarch64-linux-musl-objcopy", "--rename-section",
                        ".rodata=.data.hbxl_cookie,alloc,load,contents,data", obj], check=True)
        subprocess.run([tc / "aarch64-linux-musl-ar", "r", out.resolve(), "startup.cpp.o"], cwd=tmp, check=True)
    return out


capture, app_dir = Path(sys.argv[1]), Path(sys.argv[2])
args = capture.read_text().splitlines()
objects = []
for a in args:
    if a.startswith("-"):
        continue
    p = Path(a)
    if p.suffix in (".o", ".a"):
        if p.name.startswith("libRuntime.") and p.suffix == ".a":
            p = writable_cookie(p, capture.parent)
        objects.append(str(p.resolve()))
# Optional third argument: a JSON file whose fields override the defaults below
# (top level replaced key by key; "linux" merged; "linux.ldflags"/"sources" appended).
base = json.loads(Path(sys.argv[3]).read_text()) if len(sys.argv) > 3 else {}

manifest = {
    "id": "aot-hello",
    "name": "AOT Hello",
    "description": "osu! port test: .NET NativeAOT on the Linux personality",
    "version": "0.1.0",
    "abi": 3,
    "kind": "native",
    "sources": ["../shim/hbxl_dotnet_shim.c"],
    "linux": {
        "code_base": "0x0c000000",
        "data_base": "0x0e000000",
        "objects": objects,
        # The NativeAOT runtime archives expect these from the Linux toolchain.
        "ldflags": ["-Wl,--defsym,__xmknod=mknod", "-pthread", *fs_host_flags()],
        "libs": ["-lm"],
    },
    "limits": {"heap_bytes": 65536, "stack_bytes": 1572864, "frames": 0},
}
# "exclude_objects": basenames of captured link inputs to drop, e.g. libstdc++compat.a
# (NativeAOT's stand-in for libstdc++) when the app links the real -lstdc++.
exclude = set(base.pop("exclude_objects", []))
if exclude:
    manifest["linux"]["objects"] = [o for o in manifest["linux"]["objects"] if Path(o).name not in exclude]
for key, value in base.items():
    if key == "linux":
        for lk, lv in value.items():
            if lk in ("ldflags", "objects", "libs"):
                manifest["linux"][lk] = manifest["linux"].get(lk, []) + lv
            else:
                manifest["linux"][lk] = lv
    elif key == "sources":
        manifest["sources"] = manifest["sources"] + value
    else:
        manifest[key] = value
ilc = f"/{manifest['id']}.o"
if not any(o.endswith(ilc) for o in objects):
    sys.exit(f"make-manifest: ILC object {ilc} not found in captured link line")

app_dir.mkdir(parents=True, exist_ok=True)
(app_dir / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
print(f"make-manifest: {len(objects)} link inputs -> {app_dir / 'manifest.json'}")
