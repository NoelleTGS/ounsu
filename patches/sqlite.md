# SQLite (`src/native/sqlite-dl`)

The SQLite 3.50.4 amalgamation, unmodified: <https://www.sqlite.org/2025/sqlite-amalgamation-3500400.zip>
(sha256 `1d3049dd0f830a025a53105fc79fd2ab9431aea99e137809d064d8ee8356b032`), unzipped into `src/native/sqlite-dl`.

osu! reads databases from older lazer versions through SQLitePCLRaw, which binds `e_sqlite3`.
`sh tools/build-native.sh sqlite` compiles it into `src/native/sqlite-dl/build-lx/libe_sqlite3.a` with the options of
SQLitePCLRaw's own `e_sqlite3` build (foreign keys on by default, column metadata, FTS4/FTS5, R*Tree, snapshots, math
functions, no memory-mapped I/O). The C# side links it with `DirectPInvoke` (`console/osu-game/osu-game.csproj`).

```sh
mkdir -p src/native/sqlite-dl && cd src/native/sqlite-dl
curl -LO https://www.sqlite.org/2025/sqlite-amalgamation-3500400.zip
sha256sum sqlite-amalgamation-3500400.zip   # 1d3049dd...
unzip sqlite-amalgamation-3500400.zip
```
