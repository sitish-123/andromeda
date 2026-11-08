#!/usr/bin/env bash
# Files v2 FB-SPEED bench (.agent/DESIGN.md "Files v2" speed budgets) on a folder of 10 000 entries. Prints each
# number next to its budget; release budgets, so a debug build reports and warns but fails only on correctness.
# Fails when:
#   - the window draws no first frame or the folder never lists its 10 000 names,
#   - a fast scroll of the list view or a sort reports missed frames (DESK_FRAME_STATS),
#   - filtering does not narrow the folder.
# Numbers and the app log go to $OUT (default ./artifacts/files-speed).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/files-speed}
source "$(dirname "$0")/lib.sh"
FILES=${FILES:-$ROOT/apps/files/build/andromeda-files}
POINTER=$ROOT/compositor/build-debug/tests/pointer-client
APP=dev.andromeda.Files
HOME_DIR=$RUNTIME/home
SRC=$HOME_DIR/Big
COUNT=${COUNT:-10000}
boot_headless 1
mkdir -p "$OUT"

fail() { echo "FAIL: $*"; exit 1; }
mkdir -p "$SRC"
python3 -I - "$SRC" "$COUNT" <<'PY'
import os, sys
root, count = sys.argv[1], int(sys.argv[2])
kinds = [".txt", ".pdf", ".png", ".md", ".ods", ".zip"]
for i in range(count):
    if i % 50 == 0:
        os.mkdir(os.path.join(root, f"folder {i:05d}"))
    else:
        with open(os.path.join(root, f"file {i} report{kinds[i % len(kinds)]}"), "w") as f:
            f.write("x" * (i % 97))
PY

bus_info=$(dbus-daemon --config-file="$ROOT/tests/e2e/private-bus.conf" --fork --print-address=1 --print-pid=1)
BUS=$(sed -n 1p <<< "$bus_info")
BUS_PID=$(sed -n 2p <<< "$bus_info")
trap 'kill "$BUS_PID" 2> /dev/null || true; kill $(jobs -p) 2> /dev/null || true; wait 2> /dev/null; rm_runtime' EXIT
run env DBUS_SESSION_BUS_ADDRESS="$BUS" HOME="$HOME_DIR" XDG_STATE_HOME="$HOME_DIR/.local/state" \
  XDG_CONFIG_HOME="$HOME_DIR/.config" LC_ALL=C.UTF-8 NOCTALIA_LOG_LEVEL=debug DESK_FRAME_STATS=1 DESK_MOTION_TRACE=1 \
  DESK_FILES_GEOMETRY=1 "$FILES" "$SRC" > "$OUT/files.log" 2>&1 &

wait_log() {
  for _ in $(seq "${3:-100}"); do
    grep -qE "$2" < <(tail -n +"$(($1 + 1))" "$OUT/files.log") && return 0
    sleep 0.05
  done
  return 1
}
mark() { wc -l < "$OUT/files.log"; }
keys() { run "$POINTER" 1280 720 keyboard-only tap 119 pause 100 "$@" > /dev/null; }
win() { run "$ANDROMEDA" windows --json | jq -c "[.. | objects | select(.app_id? == \"$APP\")][0] // empty"; }
num() { grep -E "$1" "$OUT/files.log" | tail -n 1 | grep -oE "$2" | grep -oE '[0-9.]+' | head -n 1; }
mem() { # label: RSS and PSS of the Files process (smaps_rollup), MB
  local pid rss pss
  pid=$(pgrep -n -f -- "$SRC") || return 0
  rss=$(awk '/^Rss:/ {print int($2 / 1024)}' "/proc/$pid/smaps_rollup")
  pss=$(awk '/^Pss:/ {print int($2 / 1024)}' "/proc/$pid/smaps_rollup")
  printf '%-28s %10s MB   PSS %s MB\n' "memory $1" "$rss" "$pss" | tee -a "$OUT/numbers.txt"
}
report() { # name value budget unit
  local verdict=ok
  awk -v v="$2" -v b="$3" 'BEGIN { exit !(v > b) }' && verdict="OVER (release budget)"
  printf '%-28s %10s %s   budget %s %s   %s\n' "$1" "$2" "$4" "$3" "$4" "$verdict" | tee -a "$OUT/numbers.txt"
}
echo "# $FILES ($(meson introspect --buildoptions "$(dirname "$FILES")" 2> /dev/null | jq -r '.[] | select(.name == "buildtype") | .value' || echo unknown) build; budgets are for release)" > "$OUT/numbers.txt"

wait_log 0 "first frame at" 200 || fail "the window drew no first frame"
wait_log 0 "listed [0-9]+ in" 200 || fail "the folder did not list"
wait_log 0 "statted [0-9]+ in" 400 || fail "the folder stats did not finish"
listed=$(grep -oE "listed [0-9]+ in" "$OUT/files.log" | tail -n 1 | grep -oE '[0-9]+')
[[ $listed -ge $COUNT ]] || fail "listed $listed of $COUNT entries"
for _ in $(seq 100); do
  w=$(win)
  [[ -n $w && $(jq .w <<< "$w") -gt 0 ]] && break
  sleep 0.05
done
WX=$(jq .x <<< "$w"); WY=$(jq .y <<< "$w"); WW=$(jq .w <<< "$w"); WH=$(jq .h <<< "$w")
run "$ANDROMEDA" settle > /dev/null

report "window first frame" "$(num 'first frame at' 'at [0-9]+ ms')" 120 ms
# The listing result waits behind the first frame on the main thread, so its GL program build adds to both numbers.
report "  of it: GL program build" "$(num 'first frame at' 'built in [0-9.]+ ms')" 120 ms
report "$COUNT names listed" "$(num 'listed [0-9]+ in' 'in [0-9.]+ ms')" 150 ms
mem "after open"
report "$COUNT stats read" "$(num 'statted [0-9]+ in' 'in [0-9.]+ ms')" 1000 ms

# List view, then a fast wheel scroll over it: no missed frames, p99 CPU per frame under one 60 Hz frame.
m=$(mark); keys mod control tap 3 mod none
wait_log "$m" "settle files.view " || fail "Ctrl+2 did not switch to the list"
cx=$((WX + WW * 2 / 3)); cy=$((WY + WH / 2))
m=$(mark)
scroll=(move "$cx" "$cy")
# Wheel notches: a wheel scroll runs on the scroll spring, which DESK_FRAME_STATS measures (a finger scroll does not).
for _ in $(seq 40); do scroll+=(notch 1 pause 8); done
scroll+=(pause 600)
run "$POINTER" 1280 720 "${scroll[@]}" > /dev/null
run "$ANDROMEDA" settle > /dev/null
wait_log "$m" "anim .* frames" 100 || fail "the scroll reported no frame stats"
scroll_p99=$(tail -n +"$((m + 1))" "$OUT/files.log" | grep -oE "anim .* p99_cpu_ms [0-9.]+" | grep -oE '[0-9.]+$' | sort -g | tail -n 1)
scroll_missed=$(tail -n +"$((m + 1))" "$OUT/files.log" | grep -oE "anim .* missed [0-9]+" | grep -oE '[0-9]+$' | sort -n | tail -n 1)
report "scroll p99 CPU per frame" "$scroll_p99" 16.7 ms
report "scroll missed frames" "$scroll_missed" 0 ""

# Sort by size then name (the list header), and a 4-letter type-to-filter.
m=$(mark); keys mod control tap 33 mod none tap 19 tap 18 tap 25 tap 24    # Ctrl+F, r e p o
wait_log "$m" 'filter "repo" shows' || fail "the filter did not run"
filter_ms=$(tail -n +"$((m + 1))" "$OUT/files.log" | grep -oE "filter took [0-9.]+" | grep -oE '[0-9.]+$' | sort -g | tail -n 1)
report "type-to-filter per key" "$filter_ms" 16 ms
open_y=$(grep -E "geometry sort-header Size " "$OUT/files.log" | tail -n 1 | awk '{print $NF}')
esc=$(mark); keys tap 1 tap 1                   # Escape clears, Escape leaves the field
for key in Size Name; do
  # The header moves up when the filter field closes; a line logged between the two Escapes still has the old y.
  hy=""
  for _ in $(seq 40); do
    read -r hx hy <<< "$(tail -n +"$((esc + 1))" "$OUT/files.log" | grep -E "geometry sort-header $key " | tail -n 1 |
      awk '{print $(NF-1), $NF}')"
    [[ -n $hy && $hy != "$open_y" ]] && break
    sleep 0.05
  done
  [[ -n ${hx:-} ]] || fail "no geometry for the $key column header"
  m=$(mark); run "$POINTER" 1280 720 move $((WX + hx)) $((WY + hy)) click 272 > /dev/null
  wait_log "$m" "sort took" || fail "a press on the $key header did not sort"
done
sort_ms=$(grep -oE "sort took [0-9.]+" "$OUT/files.log" | grep -oE '[0-9.]+$' | sort -g | tail -n 1)
report "sort $COUNT (worst)" "$sort_ms" 16 ms

mem "after scroll, filter, sort"
grep -E "anim .* missed [1-9]" "$OUT/files.log" && echo "WARN: an animation missed frames (debug build)"
echo "pass files_speed"
