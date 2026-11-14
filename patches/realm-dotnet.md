# Realm .NET (`src/native/realm-dotnet`)

Upstream: [realm/realm-dotnet](https://github.com/realm/realm-dotnet) `20.1.0` (`realm-dotnet.base`), with realm-core as a
submodule under `wrappers/realm-core`. Patch: `realm-dotnet.diff`.

osu! keeps its beatmap, score and skin database in Realm. The C# part comes from NuGet; this checkout builds the native
`realm-wrappers` + realm-core as static musl libraries for the island (`wrappers/build-hbx/`; the build from the
HBLauncher version is reused unchanged). Changes are under the
`HBX_SWITCH2` define.

## Changes

| File | Change |
|---|---|
| `wrappers/src/CMakeLists.txt` | `realm-wrappers` as a STATIC library (no shared libraries on the console) |
| `wrappers/realm-core/src/realm/util/file.cpp` | `File::MapBase::try_reserve` takes the Windows path: the host can't place a mapping inside a reserved range (no `MAP_FIXED`; heap memory can't be put at a chosen address), so Realm falls back to one plain mapping per section, served by the host's shared-file mappings |
| `wrappers/realm-core/src/realm/object-store/impl/generic/external_commit_helper.cpp/.hpp` | realm-core uses its "generic" commit helper (HBLauncher's runtime had no epoll/FIFO for it; the kaiHEN host has epoll, but the build is unchanged), whose `notify_others()` is a no-op upstream. Realm's notifier relies on it: a listener registered while nothing was being written (osu!'s song select subscribing to beatmap sets) never got its first results, so song select stayed empty. `notify_others()` now wakes the listener, and shutdown keeps waking it until it exits |

Realm also needs three things from the host, which `lx/host` provides: `get_robust_list` (musl probes it before Realm
may use robust mutexes; `lx_syscall.c`), `MAP_SHARED` file mappings (one shared buffer per file, `lx_fd.c`) and file
locks (`flock` is granted: one process). Its callbacks into managed code go through the NativeAOT delegate thunks in
`lx/dotnet`.

## Restoring

```sh
git -C src/native/realm-dotnet checkout -f a4133ff59429949d30c6a7d184768c699bda5655
git -C src/native/realm-dotnet submodule update --init --recursive
git -C src/native/realm-dotnet apply --whitespace=nowarn patches/realm-dotnet.diff
```
