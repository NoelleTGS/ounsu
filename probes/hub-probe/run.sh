#!/bin/sh
# Local metadata-hub test (see README.md). Usage: sh run.sh [client binary]
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
CLIENT=${1:-$ROOT/console/osu-game/obj/desktop-audio/osu-game}
PORT=${HUB_PROBE_PORT:-5123}

# Debug configuration: keeps osu.Game's obj/Release (the console and desktop NativeAOT builds) untouched.
mkdir -p "$HERE/server/out"
dotnet publish "$HERE/server/hub-server.csproj" -c Debug -o "$HERE/server/out" > "$HERE/server/out/build.log" 2>&1 || {
    grep -E " error " "$HERE/server/out/build.log" | sort -u; echo "hub-probe: server build failed (server/out/build.log)"; exit 1; }

"$HERE/server/out/hub-server" --urls "http://127.0.0.1:$PORT" > "$HERE/server/out/server.log" 2>&1 &
SERVER=$!
trap 'kill $SERVER 2>/dev/null' EXIT
for i in $(seq 50); do grep -q "hub-server: ready" "$HERE/server/out/server.log" && break; sleep 0.2; done

# Safety (2026-11-10: a stale client binary without the test hook started the real game on the user's own osu! folder
# and account). The client must be newer than the sources of the hook, it gets an empty data folder of its own, and it
# is killed if it runs long.
for src in "$ROOT/console/osu-game/HubSelfTest.cs" "$ROOT/console/osu-game/Program.cs"; do
    [ "$CLIENT" -nt "$src" ] || { echo "hub-probe: $CLIENT is older than $src: rebuild it first"; exit 1; }
done
DATA=$(mktemp -d)
trap 'kill $SERVER 2>/dev/null; rm -rf "$DATA"' EXIT

status=0
XDG_DATA_HOME="$DATA" XDG_CONFIG_HOME="$DATA" SDL_VIDEODRIVER=dummy \
OSU_HBXL_HUB_LIVETEST="http://127.0.0.1:$PORT" DOTNET_DefaultStackSize=100000 \
    LD_LIBRARY_PATH="$ROOT/src/native/ffmpeg/build-desktop/lib" timeout -s KILL 120 "$CLIENT" > "$DATA/client.log" 2>&1 || status=1
grep -a "hub-livetest\|hub protocol" "$DATA/client.log" || status=1
grep -aq "hub-livetest: done, 0 failure" "$DATA/client.log" || status=1
echo "--- server log"
cat "$HERE/server/out/server.log"
exit $status
