#!/usr/bin/env bash
# Files FB-VIEWOPTS (FILES-UI-SPEC §16): each folder keeps its own view, and a KDE service menu runs on the selection.
# Fails when:
#   - Ctrl+2 in Alpha or Ctrl+1 in Beta does not save that folder's view,
#   - Beta does not open in its own (icons) view after Alpha's list, or going back to Alpha or opening Files again
#     in Alpha does not bring the list view back,
#   - Ctrl+J does not open View Options,
#   - the item menu has no Actions › row for a service menu, or the row does not run on the selection,
#   - Group by Kind does not order Gamma into Folders, Text, Images, File Extensions off or the group is not saved
#     with the folder, the list header menu does not turn on Dimensions, or the Dimensions cell is not read.
# Runs at output scale 2: view-options.png is the 2x popover shot for FILES-UI-SPEC §16.
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/files-viewopts}
source "$(dirname "$0")/lib.sh"
FILES=${FILES:-$ROOT/apps/files/build/andromeda-files}
POINTER=$ROOT/compositor/build-debug/tests/pointer-client
APP=dev.andromeda.Files
HOME_DIR=$RUNTIME/home
A=$HOME_DIR/Alpha
B=$HOME_DIR/Beta
G=$HOME_DIR/Gamma
boot_headless 1
printf '\n[output.HEADLESS-1]\nmode = "2560x1440@60"\nscale = 2\n' >> "$RUNTIME/andromeda.toml"
mkdir -p "$OUT" "$A" "$B" "$G/sub" "$HOME_DIR/.local/share/kio/servicemenus"
echo x > "$G/doc.txt"
python3 -I - "$G/pic.png" <<'PY'
import sys, zlib, struct
w, h = 64, 48
chunk = lambda k, d: struct.pack(">I", len(d)) + k + d + struct.pack(">I", zlib.crc32(k + d))
raw = b"".join(b"\0" + bytes((40, 90, 200)) * w for _ in range(h))
open(sys.argv[1], "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                       + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
PY
fail() { echo "FAIL: $*"; exit 1; }
echo one > "$A/note.txt"; echo two > "$A/other.txt"; echo three > "$B/b.txt"
MARKS=$OUT/service-marks
rm -f "$MARKS"
cat > "$HOME_DIR/.local/share/kio/servicemenus/e2e.desktop" <<EOF
[Desktop Entry]
Type=Service
MimeType=text/plain;
Actions=mark
X-KDE-Submenu=E2E

[Desktop Action mark]
Name=Mark for E2E
Exec=sh -c 'for f in "\$@"; do echo "\$f" >> $MARKS; done' _ %F
EOF
chmod +x "$HOME_DIR/.local/share/kio/servicemenus/e2e.desktop"

bus_info=$(dbus-daemon --config-file="$ROOT/tests/e2e/private-bus.conf" --fork --print-address=1 --print-pid=1)
BUS=$(sed -n 1p <<< "$bus_info")
BUS_PID=$(sed -n 2p <<< "$bus_info")
trap 'kill "$BUS_PID" 2> /dev/null || true; kill $(jobs -p) 2> /dev/null || true; wait 2> /dev/null; rm_runtime' EXIT
run "$ANDROMEDA" msg config-reload > /dev/null
for _ in $(seq 100); do run "$ANDROMEDA" outputs --json | jq -e ".[0].scale == 2" > /dev/null && break; sleep 0.05; done

LOG=$OUT/files.log
start() { # folder
  : > "$LOG"
  run env DBUS_SESSION_BUS_ADDRESS="$BUS" HOME="$HOME_DIR" XDG_DATA_HOME="$HOME_DIR/.local/share" \
    XDG_STATE_HOME="$HOME_DIR/.local/state" XDG_CONFIG_HOME="$HOME_DIR/.config" LC_ALL=C.UTF-8 ANDROMEDA_LOG_LEVEL=debug \
    DESK_FILES_GEOMETRY=1 "$FILES" "$1" >> "$LOG" 2>&1 &
  FILES_PID=$!
  for _ in $(seq 100); do
    w=$(run "$ANDROMEDA" windows --json | jq -c "[.. | objects | select(.app_id? == \"$APP\")][0] // empty")
    [[ -n $w && $(jq .w <<< "$w") -gt 0 ]] && break
    sleep 0.05
  done
  [[ -n ${w:-} ]] || fail "the Files window did not map"
  WX=$(jq .x <<< "$w"); WY=$(jq .y <<< "$w")
  run "$ANDROMEDA" settle > /dev/null
}
mark() { wc -l < "$LOG"; }
wait_log() { for _ in $(seq 100); do grep -qE "$2" < <(tail -n +"$(($1 + 1))" "$LOG") && return 0; sleep 0.05; done; return 1; }
keys() { run "$POINTER" 1280 720 keyboard-only tap 119 pause 100 "$@" > /dev/null; }
action() { DBUS_SESSION_BUS_ADDRESS=$BUS gdbus call --session -d "$APP" -o /dev/andromeda/Files \
  -m org.freedesktop.Application.ActivateAction "$1" "${2:-[]}" '{}' > /dev/null; }
at() { local line; line=$(grep -E "geometry $1( |$)" "$LOG" | tail -n 1); [[ -n $line ]] || fail "no geometry for $1"
  read -r x y <<< "$(awk '{print $(NF-1), $NF}' <<< "$line")"; echo "$((WX + x)) $((WY + y))"; }

start "$A"
m=$(mark); keys mod control tap 3 mod none                       # Ctrl+2: list
wait_log "$m" "view saved $A v=list" || fail "Ctrl+2 did not save the list view for Alpha"
m=$(mark); action go "[<'file://$B'>]"
wait_log "$m" "location $B\$" || fail "go did not open Beta"
wait_log "$m" "view icons" || fail "Beta did not open in its own icons view"
m=$(mark); keys mod control tap 3 mod none mod control tap 2 mod none   # Ctrl+2, Ctrl+1: icons, saved
wait_log "$m" "view saved $B v=icons" || fail "Ctrl+1 did not save the icons view for Beta"
m=$(mark); action go "[<'file://$A'>]"
wait_log "$m" "view list" || fail "Alpha did not open in its list view"
m=$(mark); keys mod control tap 36 mod none                      # Ctrl+J
wait_log "$m" "view options" || fail "Ctrl+J did not open View Options"
run "$ANDROMEDA" settle > /dev/null
run grim "$OUT/view-options.png"
keys tap 1
kill "$FILES_PID" 2> /dev/null || true; wait "$FILES_PID" 2> /dev/null || true

start "$A"
wait_log 0 "view list" || fail "Files opened again in Alpha did not bring its list view back"
kill "$FILES_PID" 2> /dev/null || true; wait "$FILES_PID" 2> /dev/null || true

# Service menu: right click note.txt (list view), Up x2 from Open = Actions (Trash, then Actions; the tag dots row
# takes no highlight), Right, Right (the E2E submenu), Enter.
start "$A"
wait_log 0 "geometry item note.txt" || fail "no geometry for note.txt"
run "$ANDROMEDA" settle > /dev/null
m=$(mark); run "$POINTER" 1280 720 move $(at "item note.txt") click 273 > /dev/null
wait_log "$m" "selected 1 of " || fail "the right click did not select note.txt"
run "$ANDROMEDA" settle > /dev/null
keys tap 103 tap 103 tap 106 pause 300 tap 106 pause 300 tap 28
wait_log "$m" "service action Mark for E2E on 1 items\$" || fail "the Actions › E2E › Mark for E2E row did not run"
for _ in $(seq 60); do [[ -s $MARKS ]] && break; sleep 0.05; done
[[ $(cat "$MARKS" 2> /dev/null) == "$A/note.txt" ]] || fail "the service menu command did not get note.txt"
kill "$FILES_PID" 2> /dev/null || true; wait "$FILES_PID" 2> /dev/null || true

# Stage 2: Group By, File Extensions, optional list columns.
start "$G"
m=$(mark); action view.group-kind
wait_log "$m" "groups Folders, Text, Images\$" || fail "Group by Kind did not order Gamma into Folders, Text, Images"
m=$(mark); action view.extensions
wait_log "$m" "extensions off" || fail "File Extensions did not turn off"
wait_log "$m" "view saved $G .*g=kind;x=0" || fail "the group and the extensions choice were not saved for Gamma"
run "$ANDROMEDA" settle > /dev/null
run grim "$OUT/groups-icons.png"
m=$(mark); keys mod control tap 3 mod none                       # Ctrl+2: list
wait_log "$m" "view list" || fail "Ctrl+2 did not switch Gamma to the list"
wait_log "$m" "geometry sort-header Name" || fail "no geometry for the Name header"
run "$ANDROMEDA" settle > /dev/null
m=$(mark); run "$POINTER" 1280 720 move $(at "sort-header Name") click 273 > /dev/null
wait_log "$m" "columns menu" || fail "a right click on the list header did not open the columns menu"
run "$ANDROMEDA" settle > /dev/null
keys tap 108 tap 28                                              # Down, Enter: Dimensions
wait_log "$m" "list columns 1\$" || fail "the header menu did not turn on Dimensions"
wait_log "$m" "media pic.png 64 × 48" || fail "the Dimensions column did not read pic.png"
run "$ANDROMEDA" settle > /dev/null
run grim "$OUT/groups-list.png"
kill "$FILES_PID" 2> /dev/null || true; wait "$FILES_PID" 2> /dev/null || true
start "$G"
wait_log 0 "groups Folders, Text, Images\$" || fail "Gamma opened again without its groups"
echo "pass files_viewopts"
