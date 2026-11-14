#!/bin/sh
# Stand-in for clang in .NET's NativeAOT link step.  Records the link inputs
# (one argument per line) for build.sh and leaves an empty output file, so the
# real link happens with HBLauncher's hbxl-build instead.
: "${HBXL_CAPTURE:?HBXL_CAPTURE not set}"
: > "$HBXL_CAPTURE"
out=
prev=
for a in "$@"; do
  printf '%s\n' "$a" >> "$HBXL_CAPTURE"
  [ "$prev" = "-o" ] && out=$a
  prev=$a
done
[ -n "$out" ] && : > "$out"
exit 0
