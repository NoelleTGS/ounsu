# Realm .NET (`src/native/realm-dotnet`)

Upstream: [realm/realm-dotnet](https://github.com/realm/realm-dotnet) `20.1.0` (`realm-dotnet.base`), with realm-core as a
submodule under `wrappers/realm-core`. Patch: `realm-dotnet.diff`.

osu! keeps its beatmap, score and skin database in Realm. The C# part comes from NuGet; this checkout builds the native
`realm-wrappers` + realm-core as static libraries for the console (`wrappers/build-hbx/`). Changes are under the
`HBX_SWITCH2` define.

## Changes

| File | Change |
|---|---|
| `wrappers/src/CMakeLists.txt` | `realm-wrappers` as a STATIC library (no shared libraries on the console) |
| `wrappers/realm-core/src/realm/util/file.cpp` | `File::MapBase::try_reserve` takes the Windows path: hbxl can't place a mapping inside a reserved range (no `MAP_FIXED`), so Realm falls back to one plain mapping per section, served by hbxl's shared-file mappings |
| `wrappers/realm-core/src/realm/object-store/impl/generic/external_commit_helper.cpp/.hpp` | hbxl has no epoll/FIFO for Realm, so realm-core uses its "generic" commit helper, whose `notify_others()` is a no-op upstream. Realm's notifier relies on it: a listener registered while nothing was being written (osu!'s song select subscribing to beatmap sets) never got its first results, so song select stayed empty. `notify_others()` now wakes the listener, and shutdown keeps waking it until it exits |

Realm also needed three runtime changes in hbxl (`HBLauncher.md`: `get_robust_list`, `MAP_SHARED` file mappings, file
locks) and the NativeAOT delegate thunks (Realm's callbacks into managed code).

## Restoring

```sh
git -C src/native/realm-dotnet checkout -f a4133ff59429949d30c6a7d184768c699bda5655
git -C src/native/realm-dotnet submodule update --init --recursive
git -C src/native/realm-dotnet apply --whitespace=nowarn ~/osu-HBLauncher/patches/realm-dotnet.diff
```
