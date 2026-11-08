#!/usr/bin/env bash
# Files responsive layout (user, 2026-11-08: "responsive layout is pure mess"): the window at five widths on the user's
# panel mode (2880x1800@120, scale 1.5), in the icons, list and Gallery views and with Get Info open. Shots go to
# $OUT/<view>-<width>.png for the side-by-side review. Fails when:
#   - the window does not reach a width (resize not applied),
#   - a header part (title, tools, search, More, window buttons) overlaps another at any width,
#   - a frame callback takes more than 50 ms after the first frame (main-thread stall).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/files-layout}
source "$(dirname "$0")/lib.sh"
FILES=${FILES:-$ROOT/apps/files/build/andromeda-files}
APP=dev.andromeda.Files
HOME_DIR=$RUNTIME/home
SRC=$HOME_DIR/Pictures
boot_headless 1
printf '\n[output.HEADLESS-1]\nmode = "2880x1800@120"\nscale = 1.5\n' >> "$RUNTIME/andromeda.toml"
mkdir -p "$OUT" "$SRC/Screenshots" "$HOME_DIR/Documents" "$HOME_DIR/A folder with a rather long name for ellipsis"
fail() { echo "FAIL: $*"; exit 1; }
for i in $(seq 1 40); do echo "$i" > "$HOME_DIR/Documents/note-$i.txt"; done
python3 -I - "$SRC/Screenshots" <<'PY'
import sys, zlib, struct
root = sys.argv[1]
w, h = 640, 400
chunk = lambda k, d: struct.pack(">I", len(d)) + k + d + struct.pack(">I", zlib.crc32(k + d))
head = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
for i in range(8):
    a = (i * 37 % 256, i * 91 % 256, i * 53 % 256)
    rows = [b"\0" + bytes(a) * w for _ in range(h)]
    open(f"{root}/Screenshot_{i:02d}_with_a_long_file_name.png", "wb").write(head + chunk(b"IDAT", zlib.compress(b"".join(rows), 1)) + chunk(b"IEND", b""))
PY

bus_info=$(dbus-daemon --config-file="$ROOT/tests/e2e/private-bus.conf" --fork --print-address=1 --print-pid=1)
BUS=$(sed -n 1p <<< "$bus_info")
BUS_PID=$(sed -n 2p <<< "$bus_info")
trap 'kill "$BUS_PID" 2> /dev/null || true; kill $(jobs -p) 2> /dev/null || true; wait 2> /dev/null; rm_runtime' EXIT
run "$ANDROMEDA" msg config-reload > /dev/null
for _ in $(seq 100); do run "$ANDROMEDA" outputs --json | jq -e '.[0].scale == 1.5' > /dev/null && break; sleep 0.05; done

LOG=$OUT/files.log
run env DBUS_SESSION_BUS_ADDRESS="$BUS" HOME="$HOME_DIR" XDG_STATE_HOME="$HOME_DIR/.local/state" \
  XDG_CONFIG_HOME="$HOME_DIR/.config" XDG_CACHE_HOME="$HOME_DIR/.cache" LC_ALL=C.UTF-8 NOCTALIA_LOG_LEVEL=debug \
  DESK_FILES_GEOMETRY=1 "$FILES" "$HOME_DIR" > "$LOG" 2>&1 &
wait_log() { for _ in $(seq "${3:-100}"); do grep -qE "$2" < <(tail -n +"$(($1 + 1))" "$LOG") && return 0; sleep 0.05; done; return 1; }
mark() { wc -l < "$LOG"; }
action() { DBUS_SESSION_BUS_ADDRESS=$BUS gdbus call --session -d "$APP" -o /dev/andromeda/Files \
  -m org.freedesktop.Application.ActivateAction "$1" "${2:-[]}" '{}' > /dev/null; }
win() { run "$ANDROMEDA" windows --json | jq -c "[.. | objects | select(.app_id? == \"$APP\")][0] // empty"; }
wait_log 0 "first frame at" 200 || fail "the window drew no first frame"
wait_log 0 "listed [0-9]+ in" 200 || fail "the folder did not list"
run "$ANDROMEDA" msg window-float-toggle > /dev/null 2>&1 || true
run "$ANDROMEDA" settle > /dev/null

# Sets the window width (logical px) through the compositor's resize action.
width() {
  # window-modify-width-right takes a fraction whose basis depends on the window mode (usable area, column): measure
  # the px each fraction step gives and correct, so the loop converges on any basis.
  local want=$1 w now before px_per=1920 frac
  for _ in 1 2 3 4 5 6; do
    now=$(win | jq .w)
    (( now >= want - 8 && now <= want + 8 )) && break
    frac=$(awk -v d=$((want - now)) -v p=$px_per 'BEGIN {f = d / p; if (f > 0.9) f = 0.9; if (f < -0.9) f = -0.9; printf "%+.4f", f}')
    before=$now
    run "$ANDROMEDA" msg "window-modify-width-right:$frac" > /dev/null
    # The client commits the new size later: wait for it, or the next step resizes from a stale width.
    for _ in $(seq 40); do now=$(win | jq .w); [[ $now -ne $before ]] && break; sleep 0.05; done
    run "$ANDROMEDA" settle > /dev/null
    now=$(win | jq .w)
    (( now != before )) && px_per=$(awk -v d=$((now - before)) -v f="$frac" 'BEGIN {printf "%d", d / f}')
  done
  now=$(win | jq .w)
  (( now >= want - 8 && now <= want + 8 )) || fail "the window is $now wide, not $want"
}

# The last header-row geometry line after mark $1: no two visible parts may overlap (1 px tolerance).
header_overlap() {
  tail -n +"$(($1 + 1))" "$LOG" | grep -oE "geometry header-row .*" | tail -n 1 | tr ' ' '\n' | grep ':' |
    awk -F: '{n[NR]=$1; x[NR]=$2; w[NR]=$3} END {for (i = 1; i <= NR; i++) for (j = i + 1; j <= NR; j++)
      if (x[i] < x[j] + w[j] - 1 && x[j] < x[i] + w[i] - 1) print n[i] " overlaps " n[j]}'
}
header_logged() { tail -n +"$(($1 + 1))" "$LOG" | grep -q "geometry header-row" || fail "no header geometry at $2"; }

shoot() { # view, width
  run "$ANDROMEDA" settle > /dev/null
  run grim "$OUT/$1-$2.png"
}

for w in 1440 1024 800 640 480; do
  m=$(mark)
  width "$w"
  shoot icons "$w"
  header_logged "$m" "$w"
  bad=$(header_overlap "$m")
  [[ -z $bad ]] || fail "at $w the header has $bad"
done
action go "[<'file://$HOME_DIR/Documents'>]"
wait_log 0 "location $HOME_DIR/Documents" || fail "Documents did not open"
for w in 1440 800 480; do width "$w"; shoot list-docs "$w"; done
action go "[<'file://$SRC/Screenshots'>]"
wait_log 0 "location $SRC/Screenshots" || fail "Screenshots did not open"
m=$(mark); action gallery "[<'file://$SRC/Screenshots/Screenshot_03_with_a_long_file_name.png'>]"
wait_log "$m" "view gallery" || fail "the Gallery did not open"
for w in 1440 800 480; do width "$w"; shoot gallery "$w"; done

stalls=$(awk 'match($0, /prepareFrame callback took [0-9.]+ms/) { n++; v = substr($0, RSTART + 27, RLENGTH - 29); if (n > 1 && v + 0 > 50) c++ } END { print c + 0 }' "$LOG")
echo "frame callbacks over 50 ms after the first: $stalls"
[[ $stalls -eq 0 ]] || fail "$stalls frame callbacks stalled the main thread over 50 ms"
echo "pass files_layout"
