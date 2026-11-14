#!/bin/sh
# Snapshot the local changes of every external checkout this port depends on into patches/,
# so this repository can restore a working state without vendoring gigabytes of sources.
#   patches/<name>.base   commit the patch applies to (git describe + full hash)
#   patches/<name>.diff   tracked changes (incl. submodules) + new source files, vs that commit
# Re-run before committing whenever src/*, ~/HBLauncher or ~/ns2jb changed.
# Restore: git -C <checkout> checkout <base hash> && git -C <checkout> apply --index? (see README.md)
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="$ROOT/patches"
mkdir -p "$OUT"

# name  path
REPOS="osu $ROOT/src/osu
osu-framework $ROOT/src/osu-framework
SDL $ROOT/src/native/SDL
realm-dotnet $ROOT/src/native/realm-dotnet
veldrid-spirv $ROOT/src/native/veldrid-spirv
openssl $ROOT/src/native/openssl
mysoundlib $ROOT/src/mysoundlib
ffmpeg $ROOT/src/native/ffmpeg
HBLauncher $HOME/HBLauncher
ns2jb $HOME/ns2jb"

# New files worth keeping: sources and small configs, never build output or logs.
keep() {
    grep -Ev '(^|/)(build[^/]*|obj|bin|node_modules|runs|out)/|\.(log|o|a|so|elf|bin|core|zip|tar|gz)$' |
    grep -E '\.(c|h|cc|cpp|hpp|cs|csproj|props|targets|py|cjs|js|mjs|json|sh|S|s|ld|txt|md|cmake|diff|patch|xml|ini|cfg)$|CMakeLists\.txt$' || true
}

echo "$REPOS" | while read -r name path; do
    [ -d "$path/.git" ] || [ -f "$path/.git" ] || { echo "skip $name ($path: not a git checkout)"; continue; }
    base=$(git -C "$path" rev-parse HEAD)
    printf '%s %s\n' "$(git -C "$path" describe --always --tags 2>/dev/null)" "$base" > "$OUT/$name.base"
    {
        git -C "$path" diff --binary --submodule=diff HEAD
        git -C "$path" ls-files --others --exclude-standard | keep | while read -r f; do
            [ -f "$path/$f" ] && [ "$(stat -c %s "$path/$f")" -le 1048576 ] || continue
            git -C "$path" diff --binary --no-index /dev/null "$f" || true
        done
    } > "$OUT/$name.diff"
    echo "$name: $(wc -l < "$OUT/$name.diff") lines vs $(cut -d' ' -f1 "$OUT/$name.base")"
done
