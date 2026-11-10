#!/usr/bin/env bash
# Files v2 FB-RENAME (mockup files-v2-live.html §14): Batch Rename of 24 photos by their EXIF date taken.
# Fails when:
#   - F2 on 24 selected photos does not open the Batch Rename sheet,
#   - the live preview does not use the EXIF DateTimeOriginal (the files' mtimes are years off on purpose),
#   - two photos taken the same day under "{taken} Kathmandu" are not marked as a conflict, or Rename runs anyway,
#   - "{taken} Kathmandu {n}" with 3 digits does not rename all 24 in one job,
#   - one Undo does not put all 24 names back.
# SCALE=2 writes the 2x sheet shots for the mockup pair. Shots and logs go to $OUT (default ./artifacts/files-batch-rename).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/files-batch-rename}
source "$(dirname "$0")/lib.sh"
FILES=${FILES:-$ROOT/apps/files/build/andromeda-files}
FILEOPS=${FILEOPS:-$ROOT/system/target/debug/andromeda-fileops}
APP=dev.andromeda.Files
HOME_DIR=$RUNTIME/home
DIR=$HOME_DIR/Kathmandu
boot_headless 1
if [[ ${SCALE:-1} == 2 ]]; then
  printf '\n[output.HEADLESS-1]\nmode = "2560x1440@60"\nscale = 2\n' >> "$RUNTIME/andromeda.toml"
  run "$ANDROMEDA" msg config-reload > /dev/null
  for _ in $(seq 100); do run "$ANDROMEDA" outputs --json | jq -e ".[0].scale == 2" > /dev/null && break; sleep 0.05; done
fi

fail() { echo "FAIL: $*"; exit 1; }
mkdir -p "$DIR"
# 24 real JPEGs with an EXIF APP1 (DateTimeOriginal): Oct 2 to Oct 24, except IMG_0415, taken the same day as IMG_0414.
python3 -I - "$DIR" <<'PY'
import io, struct, sys
from PIL import Image
folder = sys.argv[1]
def app1(date):
    s = date.encode() + b"\0"
    tiff = b"II*\0" + struct.pack("<I", 8)
    tiff += struct.pack("<HHHII", 1, 0x8769, 4, 1, 26) + struct.pack("<I", 0)
    tiff += struct.pack("<HHHII", 1, 0x9003, 2, len(s), 44) + struct.pack("<I", 0) + s
    body = b"Exif\0\0" + tiff
    return b"\xff\xe1" + struct.pack(">H", len(body) + 2) + body
for i in range(24):
    day = 2 + i - (1 if i >= 14 else 0)
    buf = io.BytesIO()
    Image.new("RGB", (64, 48), (40 + i * 8, 90, 160)).save(buf, "JPEG")
    jpg = buf.getvalue()
    with open(f"{folder}/IMG_{401 + i:04d}.jpg", "wb") as f:
        f.write(jpg[:2] + app1(f"2026:10:{day:02d} 09:30:00") + jpg[2:])
PY
touch -d '2019-01-01 12:00' "$DIR"/*.jpg
ls "$DIR" > "$OUT/names-before.txt"

bus_info=$(dbus-daemon --config-file="$ROOT/tests/e2e/private-bus.conf" --fork --print-address=1 --print-pid=1)
BUS=$(sed -n 1p <<< "$bus_info")
BUS_PID=$(sed -n 2p <<< "$bus_info")
trap 'kill "$BUS_PID" 2> /dev/null || true; kill $(jobs -p) 2> /dev/null || true; wait 2> /dev/null; rm_runtime' EXIT
env_app=(env DBUS_SESSION_BUS_ADDRESS="$BUS" HOME="$HOME_DIR" XDG_DATA_HOME="$HOME_DIR/.local/share"
  XDG_STATE_HOME="$HOME_DIR/.local/state" XDG_CONFIG_HOME="$HOME_DIR/.config")
run "${env_app[@]}" XDG_RUNTIME_DIR="$RUNTIME" "$FILEOPS" > "$OUT/fileops.log" 2>&1 &
run "${env_app[@]}" ANDROMEDA_LOG_LEVEL=debug "$FILES" "$DIR" > "$OUT/files.log" 2>&1 &

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
wait_for() { # description, test command...
  local what=$1; shift
  for _ in $(seq 100); do "$@" && return 0; sleep 0.05; done
  fail "$what"
}
logged() { grep -qF -- "$1" "$OUT/files.log"; }
preview() { grep -o 'batch preview .*' "$OUT/files.log" | tail -n 1; }
shot() { run "$ANDROMEDA" settle > /dev/null; sleep 0.4; run grim "$OUT/$1.png"; }

wait_for "the daemon is not on the bus" \
  env DBUS_SESSION_BUS_ADDRESS="$BUS" gdbus introspect --session -d org.andromeda.FileOps1 -o /org/andromeda/FileOps1
for _ in $(seq 40); do
  action edit.select-all
  grep -q "selected 24 of" "$OUT/files.log" && break
  sleep 0.05
done
action file.rename
wait_for "F2 on 24 photos did not open Batch Rename" logged "batch rename 24 items"

action rename.set "[<['mode','format']>]"
action rename.set "[<['format','{taken} Kathmandu']>]"
wait_for "the preview does not read the EXIF date: $(preview)" \
  logged "IMG_0401.jpg -> 2026-10-02 Kathmandu.jpg | IMG_0402.jpg -> 2026-10-03 Kathmandu.jpg"
wait_for "the same-day pair is not a conflict: $(preview)" logged "batch preview 2 can’t be renamed"
shot rename-conflict
action rename.submit
[[ $(ls "$DIR") == "$(cat "$OUT/names-before.txt")" ]] || fail "Rename ran with a conflict"
echo "PASS live preview from EXIF, conflict blocks Rename"

action rename.set "[<['format','{taken} Kathmandu {n}']>]"
action rename.set "[<['digits','3']>]"
wait_for "the counter did not clear the conflict: $(preview)" \
  logged "batch preview 24 of 24 change: IMG_0401.jpg -> 2026-10-02 Kathmandu 001.jpg"
shot rename-ok
action rename.submit
wait_for "the batch did not rename all 24" test -f "$DIR/2026-10-24 Kathmandu 024.jpg"
[[ -f "$DIR/2026-10-15 Kathmandu 014.jpg" && -f "$DIR/2026-10-15 Kathmandu 015.jpg" ]] || fail "the same-day pair: $(ls "$DIR")"
[[ $(ls "$DIR" | grep -c '^IMG_') -eq 0 && $(ls "$DIR" | wc -l) -eq 24 ]] || fail "after the rename: $(ls "$DIR")"
wait_for "the rename was not one undoable job" logged "job rename done undoable"
[[ $(grep -c "job rename done /org" "$OUT/files.log") -eq 1 ]] || fail "the batch ran as more than one job"
echo "PASS 24 renamed in one job"

action edit.undo
wait_for "one Undo did not put the 24 names back: $(ls "$DIR" | head -3)" \
  bash -c '[[ $(ls "$1") == "$(cat "$2")" ]]' _ "$DIR" "$OUT/names-before.txt"
echo "PASS one undo restores all"
