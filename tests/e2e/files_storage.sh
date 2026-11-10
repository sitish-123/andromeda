#!/usr/bin/env bash
# Files FB-STORAGE: the Storage page scan through andromeda-fileops. Fails when:
#   - the "storage" action does not scan the home folder,
#   - the scan misses a category, the Trash, the caches or the downloads older than 90 days,
#   - a scan of 20 000 files takes longer than its budget (BUDGET_MS, default 3000).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/files-storage}
source "$(dirname "$0")/lib.sh"
FILES=${FILES:-$ROOT/apps/files/build/andromeda-files}
FILEOPS=${FILEOPS:-$ROOT/system/target/debug/andromeda-fileops}
APP=dev.andromeda.Files
HOME_DIR=$RUNTIME/home
BUDGET_MS=${BUDGET_MS:-3000}
boot_headless 1
mkdir -p "$OUT"
fail() { echo "FAIL: $*"; exit 1; }

# The scan counts blocks on disk, so the fixture files hold real data.
fill() { mkdir -p "$(dirname "$1")"; head -c "$2" /dev/urandom > "$1"; }
fill "$HOME_DIR/Pictures/a.jpg" 300000
fill "$HOME_DIR/Music/b.flac" 200000
fill "$HOME_DIR/Documents/c.pdf" 100000
fill "$HOME_DIR/.cache/app/blob" 400000
fill "$HOME_DIR/.local/share/Trash/files/old.txt" 50000
mkdir -p "$HOME_DIR/.local/share/Trash/info"
printf '[Trash Info]\nPath=%s/old.txt\nDeletionDate=2026-01-01T00:00:00\n' "$HOME_DIR" \
  > "$HOME_DIR/.local/share/Trash/info/old.txt.trashinfo"
for n in 1 2 3; do
  fill "$HOME_DIR/Downloads/old$n.zip" 60000
  touch -d "120 days ago" "$HOME_DIR/Downloads/old$n.zip"
done
fill "$HOME_DIR/Downloads/new.zip" 60000
# 20 000 small files in 200 folders: the timing part of the scan.
for d in $(seq 200); do
  mkdir -p "$HOME_DIR/Code/p$d"
  (cd "$HOME_DIR/Code/p$d" && touch $(seq -f "f%g.txt" 100))
done

bus_info=$(dbus-daemon --config-file="$ROOT/tests/e2e/private-bus.conf" --fork --print-address=1 --print-pid=1)
BUS=$(sed -n 1p <<< "$bus_info")
BUS_PID=$(sed -n 2p <<< "$bus_info")
trap 'kill "$BUS_PID" 2> /dev/null || true; kill $(jobs -p) 2> /dev/null || true; wait 2> /dev/null; rm_runtime' EXIT
env_app=(env DBUS_SESSION_BUS_ADDRESS="$BUS" HOME="$HOME_DIR" XDG_DATA_HOME="$HOME_DIR/.local/share"
  XDG_STATE_HOME="$HOME_DIR/.local/state" XDG_CONFIG_HOME="$HOME_DIR/.config" XDG_CACHE_HOME="$HOME_DIR/.cache")
"${env_app[@]}" XDG_RUNTIME_DIR="$RUNTIME" "$FILEOPS" > "$OUT/fileops.log" 2>&1 &
run "${env_app[@]}" ANDROMEDA_LOG_LEVEL=debug "$FILES" "$HOME_DIR" > "$OUT/files.log" 2>&1 &

wait_for() { local what=$1; shift; for _ in $(seq 200); do "$@" && return 0; sleep 0.05; done; fail "$what"; }
on_bus() { DBUS_SESSION_BUS_ADDRESS=$BUS gdbus introspect --session -d "$1" -o "$2" > /dev/null 2>&1; }
wait_for "the daemon is not on the bus" on_bus org.andromeda.FileOps1 /org/andromeda/FileOps1
wait_for "Files is not on the bus" on_bus "$APP" /dev/andromeda/Files
mapped() { [[ $(run "$ANDROMEDA" windows --json | jq "[.. | objects | select(.app_id? == \"$APP\")] | length") -gt 0 ]]; }
wait_for "the Files window did not map" mapped

DBUS_SESSION_BUS_ADDRESS=$BUS gdbus call --session -d "$APP" -o /dev/andromeda/Files \
  -m org.freedesktop.Application.ActivateAction storage '[]' '{}' > /dev/null
wait_for "the storage action did not scan" grep -q "storage scan " "$OUT/files.log"
line=$(grep -o "storage scan .*" "$OUT/files.log" | tail -n 1)
echo "$line"
read -r cats trash caches old ms < <(sed -E 's/storage scan ([0-9]+) categories, trash ([0-9]+), caches ([0-9]+), old downloads ([0-9]+) in ([0-9]+) ms/\1 \2 \3 \4 \5/' <<< "$line")
[[ $cats -ge 4 ]] || fail "the scan found $cats categories, expected images, audio, documents and caches at least"
[[ $trash -ge 50000 ]] || fail "the scan counted $trash bytes in the Trash"
[[ $caches -ge 400000 ]] || fail "the scan counted $caches bytes of caches"
[[ $old -eq 3 ]] || fail "the scan found $old old downloads, expected 3"
[[ $ms -le $BUDGET_MS ]] || fail "the scan took $ms ms, budget $BUDGET_MS ms"
run "$ANDROMEDA" settle > /dev/null
run grim "$OUT/storage.png"
echo "pass files_storage"
