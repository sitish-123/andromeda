#!/usr/bin/env bash
# Files FB-THUMB bench: a folder of 2 000 photos in the icons view, scrolled by wheel while their thumbnails stream in
# (decode on the thumbnail workers, upload on the main loop). Prints each number next to its budget.
# Fails when:
#   - the window draws no first frame or the folder never lists its photos,
#   - no thumbnail reaches the screen while the scroll runs,
#   - the scroll reports missed frames (DESK_FRAME_STATS).
# Numbers and the app log go to $OUT (default ./artifacts/files-thumbs).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/files-thumbs}
source "$(dirname "$0")/lib.sh"
FILES=${FILES:-$ROOT/apps/files/build/andromeda-files}
POINTER=$ROOT/compositor/build-debug/tests/pointer-client
APP=dev.andromeda.Files
HOME_DIR=$RUNTIME/home
SRC=$HOME_DIR/Photos
COUNT=${COUNT:-2000}
boot_headless 1
mkdir -p "$OUT" "$SRC"
fail() { echo "FAIL: $*"; exit 1; }

# 640 x 480 PNGs, each its own gradient, so no two decodes are the same work.
python3 -I - "$SRC" "$COUNT" <<'PY'
import sys, zlib, struct
root, count = sys.argv[1], int(sys.argv[2])
w, h = 640, 480
chunk = lambda k, d: struct.pack(">I", len(d)) + k + d + struct.pack(">I", zlib.crc32(k + d))
head = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
for i in range(count):
    a = (i * 37 % 256, i * 91 % 256, i * 53 % 256)
    rows = []
    for y in range(0, h):
        t = y / (h - 1)
        rows.append(b"\0" + bytes(int(a[c] * (1 - t) + (255 - a[c]) * t) for c in range(3)) * w)
    with open(f"{root}/IMG_{i:04d}.png", "wb") as f:
        f.write(head + chunk(b"IDAT", zlib.compress(b"".join(rows), 1)) + chunk(b"IEND", b""))
PY

bus_info=$(dbus-daemon --session --fork --print-address=1 --print-pid=1)
BUS=$(sed -n 1p <<< "$bus_info")
BUS_PID=$(sed -n 2p <<< "$bus_info")
trap 'kill "$BUS_PID" 2> /dev/null || true; kill $(jobs -p) 2> /dev/null || true; wait 2> /dev/null; rm_runtime' EXIT
run env DBUS_SESSION_BUS_ADDRESS="$BUS" HOME="$HOME_DIR" XDG_STATE_HOME="$HOME_DIR/.local/state" \
  XDG_CONFIG_HOME="$HOME_DIR/.config" XDG_CACHE_HOME="$HOME_DIR/.cache" LC_ALL=C.UTF-8 NOCTALIA_LOG_LEVEL=debug \
  DESK_FRAME_STATS=1 DESK_MOTION_TRACE=1 DESK_FILES_GEOMETRY=1 "$FILES" "$SRC" > "$OUT/files.log" 2>&1 &

wait_log() {
  for _ in $(seq "${3:-100}"); do
    grep -qE "$2" < <(tail -n +"$(($1 + 1))" "$OUT/files.log") && return 0
    sleep 0.05
  done
  return 1
}
mark() { wc -l < "$OUT/files.log"; }
win() { run "$ANDROMEDA" windows --json | jq -c "[.. | objects | select(.app_id? == \"$APP\")][0] // empty"; }
report() { # name value budget unit
  local verdict=ok
  awk -v v="$2" -v b="$3" 'BEGIN { exit !(v > b) }' && verdict=OVER
  printf '%-30s %10s %s   budget %s %s   %s\n' "$1" "$2" "$4" "$3" "$4" "$verdict" | tee -a "$OUT/numbers.txt"
}
echo "# $FILES ($(meson introspect --buildoptions "$(dirname "$FILES")" 2> /dev/null | jq -r '.[] | select(.name == "buildtype") | .value' || echo unknown) build)" > "$OUT/numbers.txt"

wait_log 0 "first frame at" 200 || fail "the window drew no first frame"
wait_log 0 "listed [0-9]+ in" 200 || fail "the folder did not list"
for _ in $(seq 100); do
  w=$(win)
  [[ -n $w && $(jq .w <<< "$w") -gt 0 ]] && break
  sleep 0.05
done
WX=$(jq .x <<< "$w"); WY=$(jq .y <<< "$w"); WW=$(jq .w <<< "$w"); WH=$(jq .h <<< "$w")
run "$ANDROMEDA" settle > /dev/null

# Two passes of 30 wheel notches down and back while decodes run: each notch shows rows whose thumbnails are new.
cx=$((WX + WW * 2 / 3)); cy=$((WY + WH / 2))
m=$(mark)
scroll=(move "$cx" "$cy")
for _ in $(seq 30); do scroll+=(notch 1 pause 30); done
for _ in $(seq 30); do scroll+=(notch -1 pause 30); done
for _ in $(seq 30); do scroll+=(notch 1 pause 30); done
scroll+=(pause 600)
run "$POINTER" 1280 720 "${scroll[@]}" > /dev/null
run "$ANDROMEDA" settle > /dev/null
wait_log "$m" "anim .* frames" 100 || fail "the scroll reported no frame stats"
uploads=$(tail -n +"$((m + 1))" "$OUT/files.log" | grep -oE "thumbnails uploaded [0-9]+" | grep -oE '[0-9]+$' | paste -sd+ | bc)
[[ ${uploads:-0} -gt 0 ]] || fail "no thumbnail reached the screen during the scroll"
p99=$(tail -n +"$((m + 1))" "$OUT/files.log" | grep -oE "anim .* p99_cpu_ms [0-9.]+" | grep -oE '[0-9.]+$' | sort -g | tail -n 1)
missed=$(tail -n +"$((m + 1))" "$OUT/files.log" | grep -oE "anim .* missed [0-9]+" | grep -oE '[0-9]+$' | paste -sd+ | bc)
worst=$(tail -n +"$((m + 1))" "$OUT/files.log" | grep -oE "thumbnails uploaded [0-9]+ in [0-9.]+" | grep -oE '[0-9.]+$' | sort -g | tail -n 1)
report "thumbnails shown in scroll" "$uploads" "$COUNT" ""
report "worst upload per frame" "${worst:-0}" 4 ms
report "scroll p99 CPU per frame" "$p99" 16.7 ms
report "scroll missed frames" "$missed" 0 ""
[[ $missed -eq 0 ]] || fail "the scroll missed $missed frames while thumbnails streamed in"
echo "pass files_thumbs"
