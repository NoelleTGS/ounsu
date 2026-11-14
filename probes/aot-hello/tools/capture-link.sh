#!/bin/sh
# Stand-in for clang in .NET's NativeAOT link step. Records the link inputs (one argument per line) in $LX_CAPTURE and
# leaves an empty output file; the real link is tools/aot-island.py + make-island.sh + the lx host.
: "${LX_CAPTURE:?LX_CAPTURE not set}"
: > "$LX_CAPTURE"
out=
prev=
for a in "$@"; do
  printf '%s\n' "$a" >> "$LX_CAPTURE"
  [ "$prev" = "-o" ] && out=$a
  prev=$a
done
[ -n "$out" ] && : > "$out"
exit 0
