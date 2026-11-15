#!/usr/bin/env python3
"""Overwrite the builder's home directory in a built object, in place, with a neutral path of the same length.

Absolute source paths end up in the island as plain strings: realm-core's assert messages (__FILE__), FFmpeg's and
OpenSSL's recorded configure lines, .NET build attributes. They name the builder's home folder (and so their user
name). Each occurrence of PREFIX is replaced by "/build" padded with slashes to the same length ("/build//////..."),
which is still a valid path to the same file under a /build root. Nothing moves, so offsets and code are untouched.

usage: scrub-paths.py <file> <prefix>     (build.sh passes the island object and $HOME)"""
import mmap
import sys

path, prefix = sys.argv[1], sys.argv[2].rstrip("/").encode()
if len(prefix) < 6 or not prefix.startswith(b"/"):
    sys.exit(f"scrub-paths: refusing prefix {prefix!r} (too short or not absolute)")
neutral = b"/build".ljust(len(prefix), b"/")

count = 0
with open(path, "r+b") as f, mmap.mmap(f.fileno(), 0) as m:
    at = m.find(prefix)
    while at != -1:
        m[at:at + len(prefix)] = neutral
        count += 1
        at = m.find(prefix, at + len(prefix))
    m.flush()
print(f"scrub-paths: {count} occurrence(s) of the build machine's home folder replaced in {path}")
