#!/bin/sh
# Packs an ounsu! release and creates it on GitHub as a draft.
#
#   sh tools/release.sh             zip, then (after you confirm) tag + push the tag + draft GitHub release
#   sh tools/release.sh --zip-only  stop after dist/ounsu-<version>.zip
#
# Uses the last build (console/osu-game/build: ounsu.nro + ounsu.release, written by build.sh). The zip, extracted to
# the SD card's root, gives:
#   switch/ounsu.nro
#   switch/ounsu/osu-resources/            extracted fresh from the ppy.osu.Game.Resources package osu! references
#   switch/ounsu/LICENSE.txt               this repository's LICENSE
#   switch/ounsu/THIRD_PARTY_NOTICES.txt   tools/gen-notices.py
# The GitHub release is left as a draft: review it on GitHub and publish it there.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

die() { echo "release.sh: $*" >&2; exit 1; }

ZIP_ONLY=0
case "${1:-}" in
    "") ;;
    --zip-only) ZIP_ONLY=1 ;;
    *) die "unknown option $1 (only --zip-only)" ;;
esac

BUILD=console/osu-game/build
[ -f "$BUILD/ounsu.nro" ] && [ -f "$BUILD/ounsu.release" ] || die "no build: run sh console/osu-game/build.sh first"
VERSION=$(sed -n 's/^version=//p' "$BUILD/ounsu.release")
COMMIT=$(sed -n 's/^commit=//p' "$BUILD/ounsu.release")
DIRTY=$(sed -n 's/^dirty=//p' "$BUILD/ounsu.release")

# ------------------------------------------------------------------------------------------------ checks
echo "$VERSION" | grep -Eq '^[0-9]{4}\.[0-9]{3,4}\.[0-9]+$' || die "version '$VERSION' isn't yyyy.Mdd.N"
[ "$DIRTY" = 0 ] || die "the build was made with uncommitted changes: commit them, then build again"
[ "$COMMIT" = "$(git rev-parse HEAD)" ] || die "the build is from commit ${COMMIT%"${COMMIT#???????}"}, not the current one: build again"
[ -z "$(git status --porcelain --untracked-files=no)" ] || die "there are uncommitted changes: commit them, then build again"
if git rev-parse -q --verify "refs/tags/$VERSION" >/dev/null; then die "$VERSION is already tagged"; fi
if [ "$ZIP_ONLY" = 0 ]; then
    command -v gh >/dev/null || die "the GitHub CLI (gh) isn't installed (or use --zip-only)"
    [ -n "$(git ls-remote --tags origin "refs/tags/$VERSION")" ] && die "$VERSION is already tagged on GitHub"
    git fetch -q origin
    [ -n "$(git branch -r --contains HEAD)" ] || die "this commit isn't on GitHub yet: push your branch first"
fi

# ------------------------------------------------------------------------------------------------ zip
STAGE="dist/ounsu-$VERSION"
ZIP="dist/ounsu-$VERSION.zip"
rm -rf "$STAGE" "$ZIP"
mkdir -p "$STAGE/switch/ounsu"

# The resources: fresh from the package osu! references, so they always match the build.
R=$(sed -n 's/.*"ppy\.osu\.Game\.Resources" Version="\([^"]*\)".*/\1/p' src/osu/osu.Game/osu.Game.csproj)
[ -n "$R" ] || die "can't find the ppy.osu.Game.Resources version in src/osu/osu.Game/osu.Game.csproj"
PKGS=$(python3 -c 'import json, sys; print(next(iter(json.load(open(sys.argv[1]))["packageFolders"])))' \
    console/osu-game/obj/project.assets.json)
DLL=$(ls "$PKGS"/ppy.osu.game.resources/"$R"/lib/*/osu.Game.Resources.dll 2>/dev/null | head -n 1)
[ -n "$DLL" ] || die "ppy.osu.Game.Resources $R isn't in $PKGS: run a full build first"
echo "release.sh: extracting osu! resources $R"
dotnet run --project tools/resource-extract -c Release -- extract \
    "$STAGE/switch/ounsu/osu-resources" "$STAGE/substitutions.xml" "$DLL" >/dev/null
# The build strips exactly the extracted resources from the image (osu-resources.substitutions.xml). If the package
# has different resources now, the .nro and the loose files wouldn't match.
cmp -s "$STAGE/substitutions.xml" console/osu-game/osu-resources.substitutions.xml ||
    die "the resources package doesn't match console/osu-game/osu-resources.substitutions.xml: see UPDATING.md (resources)"
rm "$STAGE/substitutions.xml"

cp "$BUILD/ounsu.nro" "$STAGE/switch/ounsu.nro"
cp LICENSE "$STAGE/switch/ounsu/LICENSE.txt"
python3 tools/gen-notices.py "$STAGE/switch/ounsu/THIRD_PARTY_NOTICES.txt"
(cd "$STAGE" && zip -q -r -X "../ounsu-$VERSION.zip" switch)
rm -rf "$STAGE"
echo "release.sh: $ZIP ($(du -h "$ZIP" | cut -f1), $(unzip -Z1 "$ZIP" | grep -vc '/$') files)"
[ "$ZIP_ONLY" = 1 ] && exit 0

# ------------------------------------------------------------------------------------------------ GitHub
printf 'Tag %s on %s, push the tag and create a draft release on GitHub? [y/N] ' "$VERSION" "$(git rev-parse --short HEAD)"
read -r answer
[ "$answer" = y ] || [ "$answer" = Y ] || { echo "release.sh: stopped; the zip stays in dist/"; exit 0; }
git tag -a "$VERSION" -m "ounsu! $VERSION"
git push origin "refs/tags/$VERSION"
gh release create "$VERSION" "$ZIP" --draft --verify-tag --title "ounsu! $VERSION" --generate-notes
echo "release.sh: draft created. Review it and publish it on GitHub."
