#!/usr/bin/env bash
# Files FB-BASICS: org.freedesktop.FileManager1 (Firefox "Show in Folder", Chromium downloads). Fails when:
#   - `andromeda-files --service` opens a window before a request, or does not own the name,
#   - ShowItems does not open the folder with the item selected,
#   - Shift+Del does not ask before deleting, or Escape on that sheet deletes the file,
#   - ShowFolders does not open the folder,
#   - Add to Sidebar does not pin and save the folder, or a second one does not remove it,
#   - ShowItemProperties does not open Get Info for the item, or Get Info misses the snapshot version.
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/files-filemanager1}
source "$(dirname "$0")/lib.sh"
FILES=${FILES:-$ROOT/apps/files/build/andromeda-files}
APP=dev.andromeda.Files
POINTER=$ROOT/compositor/build-debug/tests/pointer-client
HOME_DIR=$RUNTIME/home
SRC=$HOME_DIR/Work
boot_headless 1
mkdir -p "$OUT" "$SRC/Inner"
fail() { echo "FAIL: $*"; exit 1; }
echo x > "$SRC/report.txt"
# A snapper-style snapshot with an older report.txt: Get Info lists it under Previous Versions.
mkdir -p "$HOME_DIR/.snapshots/7/snapshot/Work"
echo "older text" > "$HOME_DIR/.snapshots/7/snapshot/Work/report.txt"
touch -d "3 days ago" "$HOME_DIR/.snapshots/7/snapshot/Work/report.txt"

bus_info=$(dbus-daemon --config-file="$ROOT/tests/e2e/private-bus.conf" --fork --print-address=1 --print-pid=1)
BUS=$(sed -n 1p <<< "$bus_info")
BUS_PID=$(sed -n 2p <<< "$bus_info")
trap 'kill "$BUS_PID" 2> /dev/null || true; kill $(jobs -p) 2> /dev/null || true; wait 2> /dev/null; rm_runtime' EXIT
run env DBUS_SESSION_BUS_ADDRESS="$BUS" HOME="$HOME_DIR" XDG_CONFIG_HOME="$HOME_DIR/.config" LC_ALL=C.UTF-8 \
  NOCTALIA_LOG_LEVEL=debug "$FILES" --service > "$OUT/files.log" 2>&1 &

wait_log() {
  for _ in $(seq 100); do
    grep -qE "$2" < <(tail -n +"$(($1 + 1))" "$OUT/files.log") && return 0
    sleep 0.05
  done
  return 1
}
mark() { wc -l < "$OUT/files.log"; }
call() {
  DBUS_SESSION_BUS_ADDRESS="$BUS" gdbus call --session --dest org.freedesktop.FileManager1 \
    --object-path /org/freedesktop/FileManager1 --method "org.freedesktop.FileManager1.$1" "['$2']" "" > /dev/null
}
windows() { run "$ANDROMEDA" windows --json | jq "[.. | objects | select(.app_id? == \"$APP\")] | length"; }

for _ in $(seq 100); do
  DBUS_SESSION_BUS_ADDRESS="$BUS" gdbus introspect --session --dest org.freedesktop.FileManager1 \
    --object-path /org/freedesktop/FileManager1 > /dev/null 2>&1 && break
  sleep 0.05
done
DBUS_SESSION_BUS_ADDRESS="$BUS" gdbus introspect --session --dest org.freedesktop.FileManager1 \
  --object-path /org/freedesktop/FileManager1 | grep -q ShowItemProperties || fail "the service does not own FileManager1"
[[ $(windows) -eq 0 ]] || fail "--service opened a window before a request"

m=$(mark); call ShowItems "file://$SRC/report.txt"
wait_log "$m" "location $SRC\$" || fail "ShowItems did not open $SRC"
wait_log "$m" "selected 1 of 1 in $SRC" || fail "ShowItems did not select report.txt"
# Shift+Del asks first; Escape keeps the file.
m=$(mark); run "$POINTER" 1280 720 keyboard-only tap 119 pause 100 mod shift tap 111 mod none > /dev/null
wait_log "$m" "ask delete 1 items" || fail "Shift+Del did not ask before deleting"
run "$POINTER" 1280 720 keyboard-only tap 1 > /dev/null
[[ -f $SRC/report.txt ]] || fail "Escape on the delete sheet deleted the file"

m=$(mark); call ShowFolders "file://$SRC/Inner"
wait_log "$m" "location $SRC/Inner" || fail "ShowFolders did not open Inner"

# Add to Sidebar (file.pin) on the selected folder, then again to remove it.
act() {
  DBUS_SESSION_BUS_ADDRESS="$BUS" gdbus call --session -d dev.andromeda.Files -o /dev/andromeda/Files \
    -m org.freedesktop.Application.ActivateAction "$1" '[]' '{}' > /dev/null
}
m=$(mark); call ShowItems "file://$SRC/Inner"
wait_log "$m" "selected 1 of 1 in $SRC\$" || fail "ShowItems did not select the Inner folder"
m=$(mark); act file.pin
wait_log "$m" "pinned 1 folders" || fail "Add to Sidebar did not pin Inner"
grep -q "$SRC/Inner" "$HOME_DIR/.local/share/andromeda/files/pins.json" || fail "the pin was not saved"
m=$(mark); act file.pin
wait_log "$m" "unpinned 1 folders" || fail "a second Add to Sidebar did not remove the pin"

m=$(mark); call ShowItemProperties "file://$SRC/report.txt"
wait_log "$m" "get info 1 items" || fail "ShowItemProperties did not open Get Info"
wait_log "$m" "previous versions 1" || fail "Get Info did not find the snapshot version of report.txt"
run "$ANDROMEDA" settle > /dev/null
run grim "$OUT/properties.png"
echo "pass files_filemanager1"
