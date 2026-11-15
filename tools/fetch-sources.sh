#!/bin/sh
# Fetches the external projects into src/ at the commits in patches/<name>.base and applies patches/<name>.diff:
#
#   sh tools/fetch-sources.sh [name...]   (no argument: all of them; a folder that already exists is left alone)
#
# Names: osu osu-framework mysoundlib SDL realm-dotnet veldrid-spirv openssl ffmpeg sqlite. Then build the native
# libraries (sh tools/build-native.sh) and the game (sh console/osu-game/build.sh). Several GB of downloads.
# SRC_DIR overrides the destination (default: src/ in this repository).
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=${SRC_DIR:-$ROOT/src}

die() { echo "fetch-sources: $*" >&2; exit 1; }

checkout() { # name url path
    name=$1 url=$2 dir="$SRC/$3"
    if [ -e "$dir" ]; then
        echo "fetch-sources: $name: $dir exists, skipped"
        return
    fi
    base=$(cut -d' ' -f2 "$ROOT/patches/$name.base")
    echo "fetch-sources: $name ($base)"
    mkdir -p "$dir"
    git -C "$dir" init -q
    git -C "$dir" remote add origin "$url"
    git -C "$dir" fetch -q --depth 1 origin "$base"
    git -C "$dir" checkout -q FETCH_HEAD
    if [ -f "$dir/.gitmodules" ]; then
        git -C "$dir" submodule -q update --init --recursive --depth 1
    fi
    if [ -s "$ROOT/patches/$name.diff" ]; then
        git -C "$dir" apply --whitespace=nowarn "$ROOT/patches/$name.diff"
    fi
}

sqlite() {
    dir="$SRC/native/sqlite-dl"
    if [ -e "$dir/sqlite-amalgamation-3500400/sqlite3.c" ]; then
        echo "fetch-sources: sqlite: exists, skipped"
        return
    fi
    echo "fetch-sources: sqlite"
    mkdir -p "$dir"
    curl -fsSL -o "$dir/sqlite.zip" https://www.sqlite.org/2025/sqlite-amalgamation-3500400.zip
    echo "1d3049dd0f830a025a53105fc79fd2ab9431aea99e137809d064d8ee8356b032  $dir/sqlite.zip" | sha256sum -c --quiet ||
        die "sqlite.zip has the wrong checksum"
    (cd "$dir" && unzip -q sqlite.zip)
}

fetch() {
    case "$1" in
        osu) checkout osu https://github.com/ppy/osu.git osu ;;
        osu-framework) checkout osu-framework https://github.com/ppy/osu-framework.git osu-framework ;;
        mysoundlib) checkout mysoundlib https://github.com/hwsmm/mysoundlib.git mysoundlib ;;
        SDL) checkout SDL https://github.com/libsdl-org/SDL.git native/SDL ;;
        realm-dotnet) checkout realm-dotnet https://github.com/realm/realm-dotnet.git native/realm-dotnet ;;
        veldrid-spirv) checkout veldrid-spirv https://github.com/ppy/veldrid-spirv.git native/veldrid-spirv ;;
        openssl) checkout openssl https://github.com/openssl/openssl.git native/openssl ;;
        ffmpeg) checkout ffmpeg https://github.com/FFmpeg/FFmpeg.git native/ffmpeg ;;
        sqlite) sqlite ;;
        *) die "unknown name $1" ;;
    esac
}

[ $# -gt 0 ] || set -- osu osu-framework mysoundlib SDL realm-dotnet veldrid-spirv openssl ffmpeg sqlite
for name in "$@"; do fetch "$name"; done
