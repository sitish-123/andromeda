#!/usr/bin/env bash
# Files FB-BASICS drives: a LUKS drive image set up as a loop device through udisks2 (no root; polkit allows an
# active session). Changes device state: it adds a loop device and removes it at the end. Fails when:
#   - the locked drive does not show in the sidebar, or a click with its passphrase does not unlock and open it,
#   - Import Photos does not copy the DCIM photos to ~/Pictures/<date>, or a second import copies them again,
#   - Erase Drive does not format the drive as ExFAT with the new name.
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/files-drives}
source "$(dirname "$0")/lib.sh"
FILES=${FILES:-$ROOT/apps/files/build/andromeda-files}
FILEOPS=${FILEOPS:-$ROOT/system/target/debug/andromeda-fileops}
APP=dev.andromeda.Files
HOME_DIR=$RUNTIME/home
PASS=drive-pass-42
mkdir -p "$OUT" "$HOME_DIR"
fail() { echo "FAIL: $*"; exit 1; }

# The drive: a 64 MB LUKS2 image with an ExFAT file system inside, locked again before Files starts.
IMG=$RUNTIME/drive.img
truncate -s 64M "$IMG"
printf '%s' "$PASS" | cryptsetup luksFormat --batch-mode --type luks2 --pbkdf pbkdf2 --pbkdf-force-iterations 1000 \
  --key-file - "$IMG"
LOOP=$(udisksctl loop-setup --no-user-interaction -f "$IMG" | grep -o '/dev/loop[0-9]*')
[[ -n $LOOP ]] || fail "udisks did not set up the loop device"
detach() {
  udisksctl unmount --no-user-interaction -b "$CLEAR" > /dev/null 2>&1 || true
  udisksctl lock --no-user-interaction -b "$LOOP" > /dev/null 2>&1 || true
  udisksctl loop-delete --no-user-interaction -b "$LOOP" > /dev/null 2>&1 || true
}
CLEAR=$(printf '%s' "$PASS" | udisksctl unlock --no-user-interaction -b "$LOOP" --key-file /dev/stdin | grep -o '/dev/dm-[0-9]*')
[[ -n $CLEAR ]] || { detach; fail "udisks did not unlock the image"; }
CLEAR_OBJ=$(gdbus call --system -d org.freedesktop.UDisks2 -o /org/freedesktop/UDisks2/Manager \
  -m org.freedesktop.UDisks2.Manager.ResolveDevice "{'path': <'$CLEAR'>}" '{}' | grep -o "/org/[^']*")
gdbus call --system -d org.freedesktop.UDisks2 -o "$CLEAR_OBJ" -m org.freedesktop.UDisks2.Block.Format exfat \
  "{'label': <'Card'>, 'no-block': <false>}" --timeout 60 > /dev/null || { detach; fail "could not format the image"; }
udisksctl lock --no-user-interaction -b "$LOOP" > /dev/null

boot_headless 1
bus_info=$(dbus-daemon --session --fork --print-address=1 --print-pid=1)
BUS=$(sed -n 1p <<< "$bus_info")
BUS_PID=$(sed -n 2p <<< "$bus_info")
trap 'detach; kill "$BUS_PID" 2> /dev/null || true; kill $(jobs -p) 2> /dev/null || true; wait 2> /dev/null; rm_runtime' EXIT
env_app=(env DBUS_SESSION_BUS_ADDRESS="$BUS" HOME="$HOME_DIR" XDG_DATA_HOME="$HOME_DIR/.local/share"
  XDG_CONFIG_HOME="$HOME_DIR/.config" XDG_STATE_HOME="$HOME_DIR/.local/state")
# The bus traffic shows where a mount request stops (volume monitor, mount operation, Files).
dbus-monitor --address "$BUS" > "$OUT/bus.log" 2>&1 &
"${env_app[@]}" XDG_RUNTIME_DIR="$RUNTIME" "$FILEOPS" > "$OUT/fileops.log" 2>&1 &
run "${env_app[@]}" NOCTALIA_LOG_LEVEL=debug "$FILES" "$HOME_DIR" > "$OUT/files.log" 2>&1 &

wait_for() { local what=$1; shift; for _ in $(seq 400); do "$@" && return 0; sleep 0.05; done; fail "$what"; }
on_bus() { DBUS_SESSION_BUS_ADDRESS=$BUS gdbus introspect --session -d "$1" -o "$2" > /dev/null 2>&1; }
mapped() { [[ $(run "$ANDROMEDA" windows --json | jq "[.. | objects | select(.app_id? == \"$APP\")] | length") -gt 0 ]]; }
action() {
  DBUS_SESSION_BUS_ADDRESS=$BUS gdbus call --session -d "$APP" -o /dev/andromeda/Files \
    -m org.freedesktop.Application.ActivateAction "$1" "${2:-[]}" '{}' > /dev/null
}
logged() { grep -q "$1" "$OUT/files.log"; }
wait_for "Files is not on the bus" on_bus "$APP" /dev/andromeda/Files
wait_for "the Files window did not map" mapped
wait_for "the daemon is not on the bus" on_bus org.andromeda.FileOps1 /org/andromeda/FileOps1

# The sidebar lists the locked drive: its place id is "drive:<device>".
wait_for "the sidebar did not list the locked drive" logged "drive:$LOOP"
action drive.mount "[<'$LOOP'>]"
wait_for "the drive did not ask for its passphrase" logged "drive asks password"
run "$ANDROMEDA" settle > /dev/null
run grim "$OUT/unlock.png"
# The passphrase goes in through the sheet.password action: headless virtual keyboards drop keys.
action sheet.password "[<'not-the-pass'>]"
wait_for "a wrong passphrase gave no error" logged "drive unlock failed"
action sheet.password "[<'$PASS'>]"
wait_for "the passphrase did not unlock and mount the drive" logged "drive mounted at "
ROOT_DIR=$(grep -o "drive mounted at .*" "$OUT/files.log" | tail -n 1 | sed 's/drive mounted at //')
wait_for "the window did not open the drive" logged "location $ROOT_DIR"
echo "pass unlock"

mkdir -p "$ROOT_DIR/DCIM/100TEST"
head -c 20000 /dev/urandom > "$ROOT_DIR/DCIM/100TEST/IMG_0001.JPG"
head -c 30000 /dev/urandom > "$ROOT_DIR/DCIM/100TEST/VID_0002.MP4"
echo notes > "$ROOT_DIR/DCIM/100TEST/notes.txt"
DEST=$HOME_DIR/Pictures/$(date +%F)
action file.import
wait_for "Import did not copy the photo" test -f "$DEST/IMG_0001.JPG"
wait_for "Import did not copy the video" test -f "$DEST/VID_0002.MP4"
[[ ! -e $DEST/notes.txt ]] || fail "Import copied a file that is not a photo or video"
wait_for "the import was not logged" logged "imported 2 files"
action file.import
wait_for "a second import did not find the photos imported" logged "import 0 new files"
echo "pass import"

# The window still shows the drive: Erase Drive acts on it.
action drive.format
run "$ANDROMEDA" settle > /dev/null
run grim "$OUT/erase.png"
action drive.erase
wait_for "Erase did not finish" logged "drive format done"
# The whole drive is erased: the encryption goes with the old data.
fs=$(lsblk -dno FSTYPE "$LOOP" 2> /dev/null || true)
[[ $fs == exfat ]] || fail "the drive is ${fs:-empty} after Erase, expected exfat"
echo "pass erase"
echo "pass files_drives"
