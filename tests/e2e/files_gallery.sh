#!/usr/bin/env bash
# Files FB-GALLERY chrome (DESIGN.md "Files v2 immersive Gallery"): the capsules fade 2 s after the last pointer move,
# a move brings them back, F hides and shows them; the photo grows into the freed space. Fails when:
#   - Ctrl+4 does not open the Gallery,
#   - the chrome does not hide 2 s after the pointer stops, or a move or F does not bring it back,
#   - a hide or show reports missed frames (DESK_FRAME_STATS).
# Shots of the chrome shown and hidden go to $OUT (default ./artifacts/files-gallery).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/files-gallery}
source "$(dirname "$0")/lib.sh"
FILES=${FILES:-$ROOT/apps/files/build/andromeda-files}
POINTER=$ROOT/compositor/build-debug/tests/pointer-client
HOME_DIR=$RUNTIME/home
SRC=$HOME_DIR/Photos
boot_headless 1
mkdir -p "$OUT" "$SRC"
fail() { echo "FAIL: $*"; exit 1; }

# 12 landscape PNGs, each its own gradient.
python3 -I - "$SRC" <<'PY'
import sys, zlib, struct
root = sys.argv[1]
w, h = 480, 320
chunk = lambda k, d: struct.pack(">I", len(d)) + k + d + struct.pack(">I", zlib.crc32(k + d))
head = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
for i in range(12):
    a = (i * 37 % 256, i * 91 % 256, i * 53 % 256)
    rows = [b"\0" + bytes(int(a[c] * (1 - y / (h - 1)) + (255 - a[c]) * y / (h - 1)) for c in range(3)) * w for y in range(h)]
    with open(f"{root}/IMG_{i:02d}.png", "wb") as f:
        f.write(head + chunk(b"IDAT", zlib.compress(b"".join(rows), 1)) + chunk(b"IEND", b""))
PY

bus_info=$(dbus-daemon --session --fork --print-address=1 --print-pid=1)
BUS=$(sed -n 1p <<< "$bus_info")
BUS_PID=$(sed -n 2p <<< "$bus_info")
trap 'kill "$BUS_PID" 2> /dev/null || true; kill $(jobs -p) 2> /dev/null || true; wait 2> /dev/null; rm_runtime' EXIT
run env DBUS_SESSION_BUS_ADDRESS="$BUS" HOME="$HOME_DIR" XDG_STATE_HOME="$HOME_DIR/.local/state" \
  XDG_CONFIG_HOME="$HOME_DIR/.config" XDG_CACHE_HOME="$HOME_DIR/.cache" LC_ALL=C.UTF-8 NOCTALIA_LOG_LEVEL=debug \
  DESK_FRAME_STATS=1 "$FILES" "$SRC" > "$OUT/files.log" 2>&1 &

wait_log() {
  for _ in $(seq "${3:-100}"); do
    tail -n +"$(($1 + 1))" "$OUT/files.log" | grep -qE "$2" && return 0
    sleep 0.05
  done
  return 1
}
mark() { wc -l < "$OUT/files.log"; }
keys() { run "$POINTER" 1280 720 keyboard-only tap 119 pause 100 "$@" > /dev/null; }
shot() { run "$ANDROMEDA" settle > /dev/null; run grim "$OUT/$1.png"; }
missed_since() { tail -n +"$(($1 + 1))" "$OUT/files.log" | grep -oE "anim .* missed [0-9]+" | grep -oE '[0-9]+$' | paste -sd+ | bc; }

wait_log 0 "first frame at" 200 || fail "the window drew no first frame"
wait_log 0 "listed [0-9]+ in" 200 || fail "the folder did not list"
run "$ANDROMEDA" settle > /dev/null

m=$(mark); keys mod control tap 5 mod none   # Ctrl+4
wait_log "$m" "view gallery" || fail "Ctrl+4 did not open the Gallery"
run "$POINTER" 1280 720 move 640 360 pause 50 move 650 370 > /dev/null
run "$ANDROMEDA" settle > /dev/null
shot 1-chrome-shown

m=$(mark)
wait_log "$m" "gallery chrome hidden" 80 || fail "the chrome did not hide 2 s after the pointer stopped"
m=$(($(grep -nE "gallery chrome hidden" "$OUT/files.log" | tail -n 1 | cut -d: -f1) - 1))
wait_log "$m" "anim .* frames" 60 || fail "the hide reported no frame stats"
shot 2-chrome-hidden
hide_missed=$(missed_since "$m")

m=$(mark); run "$POINTER" 1280 720 move 600 300 > /dev/null
wait_log "$m" "gallery chrome shown" || fail "a pointer move did not bring the chrome back"
wait_log "$m" "anim .* frames" 60 || fail "the show reported no frame stats"
show_missed=$(missed_since "$m")

m=$(mark); keys tap 33                        # F
wait_log "$m" "gallery chrome hidden" || fail "F did not hide the chrome"
m=$(mark); keys tap 33
wait_log "$m" "gallery chrome shown" || fail "F did not bring the chrome back"

echo "hide missed frames ${hide_missed:-0}, show missed frames ${show_missed:-0}"
[[ ${hide_missed:-0} -eq 0 && ${show_missed:-0} -eq 0 ]] || fail "the chrome motion missed frames"
echo "pass files_gallery"
