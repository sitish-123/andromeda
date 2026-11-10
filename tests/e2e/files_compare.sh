#!/usr/bin/env bash
# Files v2 FB-POWER folder compare + sync in split view (mockup files-v2-live.html §16).
# Fails when:
#   - Compare Folders does not count 2 same (same.txt, the equal folder docs), 3 different (newer.txt, older.txt and
#     the folder album, whose difference is two levels down), 1 only left and 1 only right,
#   - the compare marks do not draw on both panes, or turning compare off leaves them,
#   - Sync Left → Right does not copy onlya.txt, the newer newer.txt and, inside album, the missing 2026/c.jpg and
#     the newer 2026/b.jpg (after Replace in the conflict sheets), or touches the right side's newer older.txt;
#     the compare after the sync does not show the new state.
# SCALE=2 writes the 2x shots for the mockup pair. Shots and logs go to $OUT (default ./artifacts/files-compare).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/files-compare}
source "$(dirname "$0")/lib.sh"
FILES=${FILES:-$ROOT/apps/files/build/andromeda-files}
FILEOPS=${FILEOPS:-$ROOT/system/target/debug/andromeda-fileops}
APP=dev.andromeda.Files
HOME_DIR=$RUNTIME/home
A=$HOME_DIR/PC
B=$HOME_DIR/Phone
boot_headless 1
if [[ ${SCALE:-1} == 2 ]]; then
  printf '\n[output.HEADLESS-1]\nmode = "2560x1440@60"\nscale = 2\n' >> "$RUNTIME/andromeda.toml"
  run "$ANDROMEDA" msg config-reload > /dev/null
  for _ in $(seq 100); do run "$ANDROMEDA" outputs --json | jq -e ".[0].scale == 2" > /dev/null && break; sleep 0.05; done
fi

fail() { echo "FAIL: $*"; exit 1; }
mkdir -p "$A" "$B"
echo same > "$A/same.txt"; cp -p "$A/same.txt" "$B/same.txt"
echo "pc new" > "$A/newer.txt"; echo "phone old" > "$B/newer.txt"
touch -d '2026-01-01 10:00' "$B/newer.txt"; touch -d '2026-03-01 10:00' "$A/newer.txt"
echo "pc old" > "$A/older.txt"; echo "phone new" > "$B/older.txt"
touch -d '2026-01-01 10:00' "$A/older.txt"; touch -d '2026-03-01 10:00' "$B/older.txt"
echo a > "$A/onlya.txt"
echo b > "$B/onlyb.txt"
mkdir -p "$A/docs" "$B/docs" "$A/album/2026" "$B/album/2026"
echo doc > "$A/docs/d.txt"; cp -p "$A/docs/d.txt" "$B/docs/d.txt"
echo a > "$A/album/2026/a.jpg"; cp -p "$A/album/2026/a.jpg" "$B/album/2026/a.jpg"
echo "b pc" > "$A/album/2026/b.jpg"; echo "b phone" > "$B/album/2026/b.jpg"
touch -d '2026-01-01 10:00' "$B/album/2026/b.jpg"; touch -d '2026-03-01 10:00' "$A/album/2026/b.jpg"
echo c > "$A/album/2026/c.jpg"

bus_info=$(dbus-daemon --config-file="$ROOT/tests/e2e/private-bus.conf" --fork --print-address=1 --print-pid=1)
BUS=$(sed -n 1p <<< "$bus_info")
BUS_PID=$(sed -n 2p <<< "$bus_info")
trap 'kill "$BUS_PID" 2> /dev/null || true; kill $(jobs -p) 2> /dev/null || true; wait 2> /dev/null; rm_runtime' EXIT
env_app=(env DBUS_SESSION_BUS_ADDRESS="$BUS" HOME="$HOME_DIR" XDG_DATA_HOME="$HOME_DIR/.local/share"
  XDG_STATE_HOME="$HOME_DIR/.local/state" XDG_CONFIG_HOME="$HOME_DIR/.config")
dbus-monitor --address "$BUS" "path_namespace='/org/andromeda/FileOps1'" > "$OUT/bus.log" 2>&1 &
run "${env_app[@]}" XDG_RUNTIME_DIR="$RUNTIME" "$FILEOPS" > "$OUT/fileops.log" 2>&1 &
run "${env_app[@]}" ANDROMEDA_LOG_LEVEL=debug "$FILES" "$A" > "$OUT/files.log" 2>&1 &

win() { run "$ANDROMEDA" windows --json | jq -c "[.. | objects | select(.app_id? == \"$APP\")][0] // empty"; }
for _ in $(seq 100); do
  w=$(win)
  [[ -n $w && $(jq .w <<< "$w") -gt 0 ]] && break
  sleep 0.05
done
[[ -n ${w:-} ]] || fail "the Files window did not map"
run "$ANDROMEDA" settle > /dev/null

action() { # name [gvariant parameter list]
  DBUS_SESSION_BUS_ADDRESS=$BUS gdbus call --session -d "$APP" -o /dev/andromeda/Files \
    -m org.freedesktop.Application.ActivateAction "$1" "${2:-[]}" '{}' > /dev/null
}
seen() { [[ $(grep -c -- "$1" "$OUT/files.log") -ge $2 ]]; } # the log holds $1 at least $2 times
wait_for() { # description, test command...
  local what=$1; shift
  for _ in $(seq 100); do "$@" && return 0; sleep 0.05; done
  fail "$what"
}
logged() { grep -q -- "$1" "$OUT/files.log"; }
shot() { run "$ANDROMEDA" settle > /dev/null; sleep 0.3; run grim "$OUT/$1.png"; }
# Pixels that differ between two shots in the left and in the right half of the window.
changed() { python3 - "$OUT/$1.png" "$OUT/$2.png" "$(jq .x <<< "$w")" "$(jq .y <<< "$w")" "$(jq .w <<< "$w")" "$(jq .h <<< "$w")" <<'PY'
import sys
from PIL import Image, ImageChops
a, b = Image.open(sys.argv[1]).convert('RGB'), Image.open(sys.argv[2]).convert('RGB')
k = a.width / 1280
x, y, w, h = (int(float(v) * k) for v in sys.argv[3:7])
d = ImageChops.difference(a, b)
# The window header holds the compare button and the Sync capsule: only the panes below it count.
top = y + int(80 * k)
left = d.crop((x + int(240 * k), top, x + w // 2 + int(120 * k), y + h)).getbbox()
right = d.crop((x + w // 2 + int(130 * k), top, x + w, y + h)).getbbox()
print(int(left is not None), int(right is not None))
PY
}

wait_for "the daemon is not on the bus" \
  env DBUS_SESSION_BUS_ADDRESS="$BUS" gdbus introspect --session -d org.andromeda.FileOps1 -o /org/andromeda/FileOps1
action view.split
wait_for "Split View did not open" logged "split view on"
action go "[<'$B'>]"
wait_for "the right pane did not go to Phone" logged "location $B"
action pane.activate "[<'0'>]"
shot before
action view.compare
wait_for "the compare counts are wrong: $(grep -o 'compare same.*' "$OUT/files.log" | tail -n 1)" \
  logged "compare same 2 differ 3 only-left 1 only-right 1"
shot compare
[[ $(changed before compare) == "1 1" ]] || fail "the compare marks did not draw on both panes ($(changed before compare))"
echo "PASS compare marks"

action view.sync
wait_for "the sync did not run one job per folder" logged "sync 4 items in 2 folders"
# Two jobs ask (newer.txt at the top, album/2026/b.jpg below): each gets Replace.
# The action is enabled only once the app has the conflict: send it until the app logs the answer.
resolved() { [[ $(grep -c "conflict resolve" "$OUT/files.log") -ge $1 ]]; }
replace_until() { action conflict.replace; resolved "$1"; }
for n in 1 2; do
  wait_for "the sync did not ask about conflict $n" seen 'conflict sheet file' "$n"
  wait_for "Replace did not answer conflict $n" replace_until "$n"
done
wait_for "the sync did not copy onlya.txt" test -f "$B/onlya.txt"
wait_for "the sync did not replace newer.txt: $(cat "$B/newer.txt")" test "$(cat "$B/newer.txt")" = "pc new"
wait_for "the sync did not copy album/2026/c.jpg" test -f "$B/album/2026/c.jpg"
wait_for "the sync did not replace album/2026/b.jpg: $(cat "$B/album/2026/b.jpg")" \
  test "$(cat "$B/album/2026/b.jpg")" = "b pc"
[[ $(cat "$B/older.txt") == "phone new" ]] || fail "the sync replaced the right side's newer older.txt"
[[ ! -e $A/onlyb.txt ]] || fail "Sync Left → Right copied into the left pane"
wait_for "the compare after the sync is wrong: $(grep -o 'compare same.*' "$OUT/files.log" | tail -n 1)" \
  logged "compare same 5 differ 1 only-left 0 only-right 1"
echo "PASS sync left to right"

action view.compare
wait_for "Compare did not turn off" logged "compare off"
shot after
action view.compare
wait_for "Compare did not turn on again" seen 'compare on' 2
shot again
[[ $(changed after again) == "1 1" ]] || fail "Compare off did not take the marks away ($(changed after again))"
echo "PASS compare off"
