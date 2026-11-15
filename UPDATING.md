# Updating osu! to a newer upstream version

How to move the port to a newer osu! release. The console build doesn't use ppy's packages directly: it builds
**patched checkouts** of two repositories plus one asset package, and all three have to move together.

| Piece | Where | Pinned in | Our changes |
|---|---|---|---|
| osu! | `src/osu` ([ppy/osu](https://github.com/ppy/osu)) | `patches/osu.base` | `patches/osu.diff`, described in `patches/osu.md` |
| osu-framework | `src/osu-framework` ([ppy/osu-framework](https://github.com/ppy/osu-framework)) | `patches/osu-framework.base` | `patches/osu-framework.diff` (PR #6002 + `#if HBXL`), `patches/osu-framework.md` |
| Game resources | NuGet `ppy.osu.Game.Resources` → loose files on the SD card | `src/osu/osu.Game/osu.Game.csproj` | `tools/resource-extract`, `console/osu-game/osu-resources.substitutions.xml` |

Things you **don't** update by hand:

- **osu.Game's assembly version.** `src/osu/Directory.Build.targets` stamps the build date (`yyyy.Mdd.0`) on every build.
- **The console app's version.** It stays `0.0.0` (`console/osu-game/osu-game.csproj`) so osu! treats it as a
  self-built client. The `.nro`'s own version shown in hbmenu is `NRO_VERSION` in `console/osu-game/build.sh`: bump that
  if you want hbmenu to show the change.
- **`~/osu`**, the untouched reference checkout. `src/osu`'s `origin` points at it, so fetch from GitHub instead (below).
- The native libraries (SDL3, Realm, FFmpeg, OpenSSL, veldrid-spirv). They only need attention if the new version raises a
  package that wraps them (see step 2).

Never work around the server's refusal of self-built clients (version hash, client version, etc.) during an update.

---

## 0. Before you start

```sh
cd ~/switch2hax/ounsu
git status                       # the port repo should be clean
sh tools/export-patches.sh       # snapshot the current patched state
git diff --stat patches/         # should be empty; if not, commit it first: that is your rollback point
df -h /                          # a full build plus NuGet restores needs a few GB free
```

Write down the current bases (`cat patches/osu.base patches/osu-framework.base`): rollback goes back to them.

## 1. Pick the target: the newest release

The target is the **newest GitHub release** of ppy/osu
([github.com/ppy/osu/releases](https://github.com/ppy/osu/releases)), stable or tachyon, whichever is newer. Every
release points at a tag, and that tag is what we rebase onto.

- Stable releases are marked **Latest** on GitHub (tags like `2026.1005.0-lazer`).
- **Tachyon** releases (`2026.1110.0-tachyon`) are marked **Pre-release**. They come out more often, so the newest
  release is usually one of these.
- Don't go by the tag list alone: ppy pushes tags that never get a release, or get one days later (on 2026-11-14
  `2026.1114.0-tachyon` was a tag without a release yet, and plain `2026.1026.0` never got one). Only take a tag
  once its release is published.

Get the newest release's tag (pre-releases count, drafts don't):

```sh
gh release list -R ppy/osu --limit 5            # look at it: newest first, Latest / Pre-release
T=$(gh release list -R ppy/osu --limit 1 --json tagName -q '.[0].tagName')
echo "$T"
```

Without `gh`: `curl -s 'https://api.github.com/repos/ppy/osu/releases?per_page=1' | jq -r '.[0].tag_name'`
(also newest first, pre-releases included).

Read its release notes (on GitHub or at osu.ppy.sh/home/changelog) and look for anything that touches what we patch:
audio, online/hubs, settings, skinning or Realm migrations.

Check that the release is ahead of our current base. The base itself may be an untagged commit past an older release:

```sh
git -C src/osu fetch https://github.com/ppy/osu.git tag "$T" --no-tags   # straight from GitHub, not via origin (~/osu)
git -C src/osu merge-base --is-ancestor HEAD "$T" && echo "ahead of our base: OK"
git -C src/osu rev-list --count "$T"..HEAD      # 0 = we lose nothing; >0 = our base has commits the release lacks
```

If the release isn't ahead of our base (`rev-list` shows a number above 0), moving to it would go backwards for those
commits. Usually you wait for the next release then.

### Find the matching framework and resources versions

The osu! tag decides which osu-framework and resources it needs:

```sh
git -C src/osu show "$T":osu.Game/osu.Game.csproj \
  | grep -E 'ppy.osu.Framework"|ppy.osu.Game.Resources"|"Realm"|SignalR|MessagePack|Newtonsoft'
```

`ppy.osu.Framework` is the framework **tag** to rebase onto (framework tags have the same names as its NuGet versions).
If it's the same as `patches/osu-framework.base`, skip step 2: the framework stays as it is. Otherwise fetch it:

```sh
F=2026.1030.0                    # the ppy.osu.Framework version from above
git -C src/osu-framework fetch origin tag "$F" --no-tags   # this checkout's origin is GitHub
```

### Size up the work

```sh
# How far, and does upstream touch the files we patch?
git -C src/osu rev-list --count HEAD.."$T"
git -C src/osu diff --stat HEAD "$T" -- $(git -C src/osu diff --name-only HEAD)
git -C src/osu-framework rev-list --count HEAD.."$F"
git -C src/osu-framework diff --stat HEAD "$F" -- $(git -C src/osu-framework diff --name-only HEAD)

# Package changes (new dependencies may need NativeAOT attention)
git -C src/osu diff HEAD "$T" -- '*.csproj' | grep '^[-+].*PackageReference'
git -C src/osu-framework diff HEAD "$F" -- '*.csproj' | grep '^[-+].*PackageReference'
```

The second command of each pair lists the conflicts to expect. A handful of lines is routine; hundreds in
`APIAccess.cs`, `SDL3Window*.cs`, the audio code or `SubmittingPlayer.cs` mean real porting work.

## 2. Rebase osu-framework

Do the framework first: osu! builds against it.

```sh
cd src/osu-framework
git stash push -u -m "kaihen port"      # our changes, including the new files and the staged PR #6002 files
git checkout "$F"
git stash pop
```

If `stash pop` reports conflicts, the stash is **kept**. Resolve every file with conflict markers
(`git diff --name-only --diff-filter=U`), then `git add` them, `git reset -q` (we keep the changes unstaged) and
`git stash drop`.

Rules for resolving:

- **Keep upstream's code and re-apply ours on top.** Every console change is inside `#if HBXL` / `#if HBXL_AUDIO`
  blocks, so the `#else` branch should be upstream's *new* code, not the old code from our base.
- **`osu.Framework.csproj`:** keep upstream's new package versions and our `HbxlBuild`/`HbxlAudio` property groups and
  the FFmpeg.AutoGen 7.1.1 pin. The FFmpeg version has to match the static FFmpeg (`tools/build-ffmpeg.sh`).
- **PR #6002 (SDL3 audio)** isn't upstream yet. If upstream changed `AudioManager`, `Track`, `Sample`, `AudioThread` or
  `BassAudioManager`, check the PR on GitHub for a newer revision before merging by hand. If it has been merged
  upstream, drop our copy and keep only the `#if HBXL` bits.
- **`ppy.osu.Framework.NativeLibs`** only carries BASS and the other desktop native libraries (we link BASS stubs), so a
  bump there doesn't matter. A bump of **`ppy.SDL3-CS`** does matter: the bindings must match the SDL3 we build
  (`src/native/SDL`, a musl build reused from `~/osu-HBLauncher`). If its SDL version changes, the SDL build has to
  follow. Claude must not edit SDL's sources (see CLAUDE.md), and our SDL patches would need re-applying to the new
  version.
- **Veldrid** (`ppy.Veldrid`, `ppy.Veldrid.SPIRV`): a bump can change the shader compiler (`src/native/veldrid-spirv`,
  `src/veldrid-src`). Check the PR before assuming it's harmless.

Check that nothing was lost: every row of the tables in `patches/osu-framework.md` should still have its change.

```sh
git diff --stat "$F"            # compare with the file list in patches/osu-framework.md
grep -rn '#if HBXL' --include=*.cs osu.Framework | wc -l   # compare with the old count (git stash show / the old diff)
cd ../..
```

## 3. Rebase osu!

```sh
cd src/osu
git stash push -u -m "kaihen port"
git checkout "$T"
git stash pop
```

Resolve conflicts the same way. Notes per file:

- **`osu.Game/osu.Game.csproj`:** this one almost always conflicts. Upstream changes the `ppy.osu.Framework` version
  on the very line our patch **deletes**. Keep it deleted: osu.Game references `../../osu-framework` through the
  `ProjectReference`. Keep upstream's other new versions (resources, Realm, ...) and our `HbxlBuild`/`HbxlDetachTest`
  property groups.
- **`Online/API/APIAccess.cs`:** only the metadata hub connector may be created. Spectator and multiplayer must stay null.
- **`Online/HubClientConnector.cs`, `Online/HbxlMessagePackHubProtocol.cs`:** if the metadata hub gained new methods or
  message types, add them to `HbxlHubTypes`. Check with `OSU_HBXL_HUB_SELFTEST=1` and `probes/hub-probe`.
- **`Screens/Play/SubmittingPlayer.cs`:** keep "no submission token request" and the disabled playback-rate check.
- **`Database/RealmObjectExtensions.cs` + `HbxlRealmCopier.cs`:** if upstream added fields to Realm models
  (`BeatmapInfo`, `BeatmapSetInfo`, `ScoreInfo`, `SkinInfo`, `RulesetInfo`, ...) or changed the AutoMapper config,
  `HbxlRealmCopier` has to copy the new members too. Otherwise detached copies silently lose them. Diff the models:
  `git diff HEAD@{1} HEAD -- osu.Game/Beatmaps/*Info.cs osu.Game/Scoring/ScoreInfo.cs osu.Game/Skinning/SkinInfo.cs`.
  Then run the desktop test (`dotnet test -p:HbxlDetachTest=true`, see the HBLauncher CLAUDE.md).
- **`OsuGameBase.cs`:** keep the `HbxlFileResourceStore` swap for the resources DLL.
- Other files (`RollingCounter`, `BindableValueAccessor`, `SettingSourceAttribute`, `FrameStabilityContainer`,
  `ModelDownloader`, `ImportTask`, `WorkingBeatmapCache`, `SettingsOverlay`, the notifications connector): re-apply the
  `#if HBXL` block onto upstream's new code.

Then look for **new code NativeAOT can't run**. These are the usual causes of new crashes after an update:

```sh
git diff HEAD@{1} HEAD -- '*.cs' | grep -nE '^\+.*(MakeGenericType|MakeGenericMethod|Expression\.Lambda|\.Compile\(\)|Activator\.CreateInstance|AddDelayed\(.*repeat: ?true)'
```

- `MakeGenericType`/`MakeGenericMethod` over value types: give them a non-generic path, as in `BindableValueAccessor`.
- Repeating scheduler tasks (`AddDelayed(..., repeat: true)`) in code that runs on the console: move them to `Update()`
  ("Too many timed tasks" crash).
- New Newtonsoft collection types: `rd.xml` (step 5 takes care of it).

```sh
git diff --stat "$T"            # compare with patches/osu.md
cd ../..
```

## 4. Game resources

Skip this step if `ppy.osu.Game.Resources` didn't change. Otherwise the SD card is missing the new textures and skin
elements, and the build would still embed the new resources (the substitutions file only removes the ones it lists).

```sh
R=2026.1110.0                    # the new ppy.osu.Game.Resources version
dotnet restore console/osu-game -p:HbxlBuild=true   # downloads the package into ~/.nuget/packages
DLL=~/.nuget/packages/ppy.osu.game.resources/$R/lib/net8.0/osu.Game.Resources.dll   # check the lib/ folder name
dotnet run --project tools/resource-extract -c Release -- list "$DLL"     # optional: what's inside
rm -rf fsroot/osu-resources.new
dotnet run --project tools/resource-extract -c Release -- extract \
    fsroot/osu-resources.new console/osu-game/osu-resources.substitutions.xml "$DLL"
```

Only the main `osu.Game.Resources.dll` goes in. The `<lang>/osu.Game.Resources.resources.dll` satellites are localised
strings and stay compiled in.

Swap the folders once it worked (`mv fsroot/osu-resources fsroot/osu-resources.old && mv fsroot/osu-resources.new
fsroot/osu-resources`), then upload them to the card. With ounceshell's FTP server running on the console:

```sh
python3 tools/ftp-upload.py fsroot/osu-resources /switch/ounsu/osu-resources --host <console-ip>
```

The upload skips files whose size didn't change. A file that changed but kept its exact size would be skipped too, so
after a resources update, delete `/switch/ounsu/osu-resources` on the card first (ounceshell) and upload it fresh. Files
that upstream removed stay behind otherwise as well.

## 5. Build

```sh
sh console/osu-game/build.sh            # full build: dotnet publish + ILC, ~15 min
```

Never use `LX_SKIP_PUBLISH=1` after an update: it relinks the old managed code.

**`rd.xml` lags one build behind.** `build.sh` regenerates `console/osu-game/rd.xml` from the *previous* build's IL
before it publishes. After an update, run the full build **twice**, so the second build gets an `rd.xml` that covers the
new version's JSON types. Then check `git diff console/osu-game/rd.xml`.

Build errors to expect:

- **`CS0246`/`CS0103` in `#if HBXL` code:** upstream renamed or moved something our patch uses. Fix the patch.
- **ILC warnings about new trimming problems:** a new reflection-heavy library may need a `TrimmerRootAssembly` in
  `console/osu-game/osu-game.csproj`. Look at how Newtonsoft.Json and Realm are rooted there.
- **Island link errors (undefined symbols):** a new native dependency, or a newer runtime calling something the shims
  don't provide. `tools/unused-undef.py` helps sort out which symbols matter.

Optional, and faster to debug: the desktop NativeAOT comparison build of the same sources (HBLauncher CLAUDE.md,
"Current state"). Run it with a temporary `XDG_DATA_HOME` and a timeout, never against your real lazer data folder.

## 6. Test on the console

Send the build with nxlink and keep the log (see TESTING.md and CLAUDE.md, "Workflow"), then go through this:

- [ ] Starts, main menu at full frame rate, no new errors in the log (`grep -iE 'exception|error|fail'`).
- [ ] **Realm migration:** a new version may bump the schema. Start with the existing data folder on the card (back up
      `/switch/ounsu/home/.local/share/osu` first) and check that beatmaps, scores and skins are still there.
- [ ] Song select, a full map in each ruleset, results screen, replay.
- [ ] Audio: music, hitsounds, no crackles; seeking in song select.
- [ ] Skins: default skins load, and a saved skin layout still applies (that depends on the version stamp).
- [ ] Settings, including the **Switch 2** section; mod select (enum dropdowns).
- [ ] Online: log in, beatmap listing/download, metadata hub connects (and gets refused quietly), news/changelog.
- [ ] Import from `/switch/ounsu/import`.
- [ ] Dock/undock, Joy-Con mouse, touch, exit back to hbmenu.

## 7. Record it

```sh
sh tools/export-patches.sh               # new patches/osu.{base,diff} and osu-framework.{base,diff}
git diff --stat patches/
```

Then update the text:

- `patches/osu.md` and `patches/osu-framework.md`: the base commit/tag in the first lines, and any added or removed table rows.
- `CLAUDE.md`: `~/osu`'s commit is only the reference checkout. Mention the new `src/osu` base where the status says
  which osu! version the port runs.
- `FEASIBILITY.md`: append a dated entry (old → new tags, conflicts, anything that broke). Append, don't rewrite.
- `NRO_VERSION` in `console/osu-game/build.sh` if you want hbmenu to show it.

Commit: `git add patches console/osu-game/rd.xml console/osu-game/osu-resources.substitutions.xml *.md && git commit`.

## Rolling back

The committed patches are the old state. For each checkout:

```sh
git -C src/osu checkout -f <old osu.base hash>
git -C src/osu clean -fd -e bin -e obj      # removes the new files of the failed attempt (check with -n first)
git -C src/osu apply --whitespace=nowarn patches/osu.diff
# same for src/osu-framework with patches/osu-framework.diff
```

(`git checkout patches/` first if you already re-exported them.) For resources, put `fsroot/osu-resources.old` back
and `git checkout console/osu-game/osu-resources.substitutions.xml`. Then do a full build.
