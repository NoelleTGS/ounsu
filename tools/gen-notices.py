#!/usr/bin/env python3
"""Write THIRD_PARTY_NOTICES.txt for a release: every third-party component in ounsu.nro and the release zip, with
its licence text.

Sources: the licence files of the checkouts in src/, the installed devkit, the musl source, and for the .NET side
every NuGet package in osu-game's restore (console/osu-game/obj/project.assets.json, so run a build first). Packages
that declare a standard licence (SPDX expression) are listed under one copy of that licence; packages that ship a
licence file get it verbatim. A package with no licence information and no entry in KNOWN stops the script: check
the package's repository and add it to KNOWN.

usage: gen-notices.py <out.txt>
env:   MUSL_SRC  musl source folder with COPYRIGHT (default ~/osu-HBLauncher/toolchain-src/musl-1.2.6)"""
import json
import os
import re
import sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DKP = os.environ.get("DEVKITPRO", "/opt/devkitpro")
MUSL_SRC = os.environ.get("MUSL_SRC", os.path.expanduser("~/osu-HBLauncher/toolchain-src/musl-1.2.6"))
REPO_URL = "https://github.com/NoelleTGS/ounsu"

# Packages whose .nuspec has no usable licence information, checked against their repositories.
KNOWN = {
    "HidSharpCore": "Apache-2.0",
    "ppy.osuTK.NS20": "MIT",          # osuTK (OpenTK fork): The Open Toolkit library license, MIT-style
    "ppy.SDL2-CS": "Zlib",
    "ppy.Veldrid": "MIT", "ppy.Veldrid.MetalBindings": "MIT", "ppy.Veldrid.OpenGLBindings": "MIT",
    "ppy.Veldrid.SPIRV": "MIT",
    "ppy.Vk": "MIT",
    "Remotion.Linq": "Apache-2.0",
    "Microsoft.NETCore.Platforms": "MIT", "NETStandard.Library": "MIT",
    "Microsoft.DotNet.ILCompiler": "MIT", "runtime.linux-musl-arm64.Microsoft.DotNet.ILCompiler": "MIT",
    "Microsoft.NET.ILLink.Tasks": "MIT",
    "ppy.osu.Game.Resources": "CC-BY-NC-4.0",
}

MIT_TEXT = """Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit
persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE."""

BSD2_TEXT = """Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following
   disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the
   following disclaimer in the documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE."""

ZLIB_TEXT = """This software is provided 'as-is', without any express or implied warranty. In no event will the authors be held
liable for any damages arising from the use of this software.

Permission is granted to anyone to use this software for any purpose, including commercial applications, and to
alter it and redistribute it freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not claim that you wrote the original software.
   If you use this software in a product, an acknowledgment in the product documentation would be appreciated but is
   not required.
2. Altered source versions must be plainly marked as such, and must not be misrepresented as being the original
   software.
3. This notice may not be removed or altered from any source distribution."""


def src(*parts):
    return os.path.join(ROOT, "src", *parts)


def read(path):
    if not os.path.isfile(path):
        sys.exit(f"gen-notices: missing licence file {path}")
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read().strip()


# Full texts of the licences named by SPDX id, from files already on this machine.
TEXTS = {
    "MIT": lambda: MIT_TEXT,
    "BSD-2-Clause": lambda: BSD2_TEXT,
    "Zlib": lambda: ZLIB_TEXT,
    "Apache-2.0": lambda: read(src("native", "realm-dotnet", "LICENSE")),
    "LGPL-2.1-only": lambda: read(src("mysoundlib", "LICENSE")),
    "LGPL-2.1-or-later": lambda: read(src("mysoundlib", "LICENSE")),
    "LGPL-3.0-or-later": lambda: read(src("native", "ffmpeg", "COPYING.LGPLv3")) + "\n\n" + "-" * 78 + "\n\n"
                                 + read(src("native", "ffmpeg", "COPYING.GPLv3")),
}

out = []
w = out.append
RULE = "=" * 118


def section(title):
    w(""); w(RULE); w(title); w(RULE); w("")


def component(name, licence, files=(), note=None):
    w("-" * 118)
    w(f"{name}  ({licence})")
    if note:
        w(note)
    w("-" * 118)
    for f in files:
        w(""); w(read(f))
    w("")


# ---------------------------------------------------------------------------------------------------- header
w("ounsu! - third-party notices")
w("")
w("ounsu! is an unofficial port of osu! (lazer) to the Nintendo Switch 2. Its own code is under the MIT licence")
w(f"(LICENSE, {REPO_URL}). ounsu.nro and the release zip also contain the third-party components below, each under")
w("its own licence.")
w("")
w("LGPL components (FFmpeg, mysoundlib, FFmpeg.AutoGen, SoundTouch.Net) are linked statically. Their source code, the")
w("changes made for ounsu! (patches/), and everything needed to rebuild ounsu.nro with a modified version of them are")
w(f"available at {REPO_URL}: see its README (\"Build it yourself\") and patches/*.md for the exact upstream versions.")

# ---------------------------------------------------------------------------------------------------- game + resources
section("1. osu! and its resources")
component("osu! and osu-framework, by ppy Pty Ltd and contributors", "MIT",
          [src("osu", "LICENCE"), src("osu-framework", "LICENCE")],
          "https://github.com/ppy/osu, https://github.com/ppy/osu-framework. \"osu!\" and \"ppy\" are trademarks of "
          "ppy Pty Ltd;\nounsu! is not affiliated with or endorsed by ppy.")
component("osu! resources (switch/ounsu/osu-resources/ in the release zip), by ppy Pty Ltd", "CC BY-NC 4.0", (),
          "From the ppy.osu.Game.Resources NuGet package (https://github.com/ppy/osu-resources), extracted to loose files\n"
          "without changes to their content. Licensed under Creative Commons Attribution-NonCommercial 4.0\n"
          "International: https://creativecommons.org/licenses/by-nc/4.0/legalcode. Non-commercial use only.")

# ---------------------------------------------------------------------------------------------------- native
section("2. Native libraries linked into ounsu.nro")
component("SDL 3, by Sam Lantinga and contributors", "zlib", [src("native", "SDL", "LICENSE.txt")],
          "https://libsdl.org. The hbx audio/joystick drivers come from the Twilight Princess HBLauncher port (CC0).")
component("FFmpeg 7.1.1 (audio decoders only)", "LGPL-2.1-or-later",
          [src("native", "ffmpeg", "LICENSE.md"), src("native", "ffmpeg", "COPYING.LGPLv2.1")],
          "https://ffmpeg.org. Built without --enable-gpl and --enable-nonfree (tools/build-ffmpeg.sh).")
component("mysoundlib, by hwsmm", "LGPL-2.1", [src("mysoundlib", "LICENSE")], "https://github.com/hwsmm/mysoundlib")
component("OpenSSL 3.5", "Apache-2.0", [src("native", "openssl", "LICENSE.txt")], "https://www.openssl.org")
component("Realm .NET and realm-core, by MongoDB, Inc.", "Apache-2.0",
          [src("native", "realm-dotnet", "wrappers", "realm-core", "THIRD-PARTY-NOTICES")],
          "https://github.com/realm/realm-dotnet. Apache License 2.0 text: see section 4.")
component("SQLite", "public domain", (),
          "https://sqlite.org. The authors disclaim copyright to the SQLite source code; it is in the public domain.")
vs = src("native", "veldrid-spirv")
component("veldrid-spirv, by Eric Mellino and contributors (ppy's fork)", "MIT", [os.path.join(vs, "LICENSE")],
          "https://github.com/ppy/veldrid-spirv")
component("SPIRV-Cross, by the Khronos Group and ARM", "Apache-2.0", [os.path.join(vs, "ext", "SPIRV-Cross", "LICENSE")])
component("shaderc, by Google", "Apache-2.0", [os.path.join(vs, "ext", "shaderc", "LICENSE")])
component("glslang, by the Khronos Group and others", "BSD-3-Clause and others",
          [os.path.join(vs, "ext", "glslang", "LICENSE.txt")])
component("SPIRV-Tools, by the Khronos Group and others", "Apache-2.0", [os.path.join(vs, "ext", "spirv-tools", "LICENSE")])
component("SPIRV-Headers, by the Khronos Group", "MIT", [os.path.join(vs, "ext", "spirv-headers", "LICENSE")])
component("musl libc", "MIT", [os.path.join(MUSL_SRC, "COPYRIGHT")], "https://musl.libc.org")
component("GCC runtime libraries (libstdc++, libgcc)", "GPL-3.0 with the GCC Runtime Library Exception", (),
          "https://gcc.gnu.org. Under the GCC Runtime Library Exception, version 3.1, programs compiled with GCC may\n"
          "include these libraries under terms of their choice.")
lic = os.path.join(DKP, "libounce", "licenses")
component("libounce and libnx", "ISC",
          [os.path.join(lic, "libounce-ISC.txt"), os.path.join(lic, "libnx-ISC.txt")],
          "https://github.com/ouncebrew/libounce, https://github.com/switchbrew/libnx")
component("mimalloc, by Microsoft", "MIT", [os.path.join(lic, "mimalloc-MIT.txt")])
vk = os.path.join(DKP, "portlibs", "switch2", "share", "licenses", "ouncevk")
component("OunceVK (Mesa), by ouncebrew and the Mesa contributors", "MIT and others",
          [os.path.join(vk, "mesa-license.rst"), os.path.join(vk, "zlib-license.txt")],
          "https://github.com/ouncebrew/ouncevk")
component("newlib (devkitA64's C library)", "BSD-style and others", (),
          "https://sourceware.org/newlib. Licences: https://sourceware.org/newlib/COPYING.NEWLIB")
component("Mozilla CA certificate bundle (usr/local/ssl/cert.pem in the .nro)", "MPL-2.0", (),
          "Unmodified, as packaged by curl: https://curl.se/docs/caextract.html. Source and licence:\n"
          "https://www.mozilla.org/MPL/2.0/")

# ---------------------------------------------------------------------------------------------------- .NET
section("3. .NET runtime and managed libraries compiled into ounsu.nro (NativeAOT)")
assets_path = os.path.join(ROOT, "console", "osu-game", "obj", "project.assets.json")
if not os.path.isfile(assets_path):
    sys.exit("gen-notices: no project.assets.json: run a full build first")
assets = json.load(open(assets_path))
pkg_root = next(iter(assets["packageFolders"]))
target = next(v for k, v in assets["targets"].items() if "/" in k)
by_licence = defaultdict(list)
file_licences = []
ilc_notices = None
for key, entry in sorted(target.items(), key=lambda kv: kv[0].lower()):
    if entry.get("type") != "package":
        continue
    pid, ver = key.split("/")
    pdir = os.path.join(pkg_root, assets["libraries"][key]["path"])
    nuspec = next((os.path.join(pdir, f) for f in os.listdir(pdir) if f.endswith(".nuspec")), None)
    spec = open(nuspec, encoding="utf-8").read() if nuspec else ""
    url = re.search(r"<projectUrl>([^<]+)</projectUrl>", spec)
    copyright_ = re.search(r"<copyright>([^<]+)</copyright>", spec)
    authors = re.search(r"<authors>([^<]+)</authors>", spec)
    holder = (copyright_.group(1) if copyright_ else f"Copyright (c) {authors.group(1)}" if authors else "").strip()
    line = f"{pid} {ver}" + (f"  {url.group(1)}" if url else "") + (f"\n    {holder}" if holder else "")
    if pid == "runtime.linux-musl-arm64.Microsoft.DotNet.ILCompiler":
        ilc_notices = os.path.join(pdir, "THIRD-PARTY-NOTICES.TXT")
    if pid == "ppy.osu.Game.Resources":
        continue  # section 1
    expr = re.search(r'<license type="expression">([^<]+)</license>', spec)
    lfile = re.search(r'<license type="file">([^<]+)</license>', spec)
    shipped = next((os.path.join(pdir, f) for f in sorted(os.listdir(pdir))
                    if re.match(r"(licen[cs]e|copying)(\.(md|txt))?$", f, re.I)), None)
    if lfile:
        file_licences.append((line, os.path.join(pdir, lfile.group(1))))
    elif expr:
        by_licence[expr.group(1).strip()].append(line)
    elif shipped:
        file_licences.append((line, shipped))
    elif pid in KNOWN:
        by_licence[KNOWN[pid]].append(line)
    else:
        sys.exit(f"gen-notices: {pid} {ver} has no licence information: check its repository and add it to KNOWN")

w(".NET runtime (NativeAOT), by the .NET Foundation and contributors: MIT. https://github.com/dotnet/runtime")
w("Copyright (c) .NET Foundation and Contributors. MIT licence text: see section 4. Its third-party notices follow.")
w("")
if ilc_notices and os.path.isfile(ilc_notices):
    w(read(ilc_notices)); w("")

for licence in sorted(by_licence):
    w("-" * 118)
    w(f"Packages under {licence} (licence text: see section 4)")
    w("-" * 118)
    for line in by_licence[licence]:
        w(line)
    w("")
for line, path in file_licences:
    w("-" * 118)
    w(line)
    w("-" * 118)
    w(read(path)); w("")

# ---------------------------------------------------------------------------------------------------- texts
section("4. Licence texts")
needed = set(by_licence) | {"MIT", "Apache-2.0", "Zlib"}
for licence in sorted(needed):
    parts = [p.strip("() ") for p in re.split(r"\s+(?:OR|AND)\s+", licence)]
    for p in parts:
        if p not in TEXTS:
            sys.exit(f"gen-notices: no text for licence {p!r}: add it to TEXTS")
for licence in sorted({p.strip("() ") for l in needed for p in re.split(r"\s+(?:OR|AND)\s+", l)}):
    w("-" * 118)
    w(licence)
    w("-" * 118)
    w(""); w(TEXTS[licence]()); w("")

with open(sys.argv[1], "w", encoding="utf-8") as f:
    f.write("\n".join(out) + "\n")
print(f"gen-notices: {sys.argv[1]}: {sum(map(len, by_licence.values())) + len(file_licences)} packages")
