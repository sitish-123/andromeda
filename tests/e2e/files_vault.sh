#!/usr/bin/env bash
# Files FB-VAULT: gocryptfs vaults. Fails when:
#   - New Vault does not make an encrypted folder and open it at ~/Vaults/<name>,
#   - a file saved in the vault shows its name or content in the cipher folder,
#   - Lock does not unmount the vault, or a wrong password unlocks it,
#   - Unlock with the right password does not bring the file back,
#   - the guard does not take a sleep inhibitor, or does not lock the vault when logind announces sleep.
# Needs gocryptfs and FUSE (/dev/fuse).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/files-vault}
source "$(dirname "$0")/lib.sh"
FILES=${FILES:-$ROOT/apps/files/build/andromeda-files}
APP=dev.andromeda.Files
HOME_DIR=$RUNTIME/home
NAME=Private
PASSWORD=correct-horse-9
mkdir -p "$OUT" "$HOME_DIR"
fail() { echo "FAIL: $*"; exit 1; }
command -v gocryptfs > /dev/null || { echo "SKIP: gocryptfs is not installed (sudo pacman -S gocryptfs)"; exit 77; }
boot_headless 1

bus_info=$(dbus-daemon --config-file="$ROOT/tests/e2e/private-bus.conf" --fork --print-address=1 --print-pid=1)
BUS=$(sed -n 1p <<< "$bus_info")
BUS_PID=$(sed -n 2p <<< "$bus_info")
# A second private bus stands in for the system bus: the guard talks to logind there.
sys_info=$(dbus-daemon --config-file="$ROOT/tests/e2e/private-bus.conf" --fork --print-address=1 --print-pid=1)
SYSBUS=$(sed -n 1p <<< "$sys_info")
SYSBUS_PID=$(sed -n 2p <<< "$sys_info")
MOUNT=$HOME_DIR/Vaults/$NAME
cleanup() {
  fusermount3 -u -z "$MOUNT" 2> /dev/null || true
  kill "$BUS_PID" "$SYSBUS_PID" 2> /dev/null || true
  kill $(jobs -p) 2> /dev/null || true
  wait 2> /dev/null
  rm_runtime
}
trap cleanup EXIT
env_app=(env DESK_FILES_TEST=1 DBUS_SESSION_BUS_ADDRESS="$BUS" DBUS_SYSTEM_BUS_ADDRESS="$SYSBUS" HOME="$HOME_DIR"
  XDG_DATA_HOME="$HOME_DIR/.local/share" XDG_CONFIG_HOME="$HOME_DIR/.config" XDG_STATE_HOME="$HOME_DIR/.local/state")

mkfifo "$RUNTIME/logind.in"
DBUS_SYSTEM_BUS_ADDRESS=$SYSBUS python3 -I "$ROOT/tests/e2e/fake_logind.py" < "$RUNTIME/logind.in" > "$OUT/logind.log" 2>&1 &
exec 7> "$RUNTIME/logind.in"
run "${env_app[@]}" NOCTALIA_LOG_LEVEL=debug "$FILES" "$HOME_DIR" > "$OUT/files.log" 2>&1 &

wait_for() { local what=$1; shift; for _ in $(seq 200); do "$@" && return 0; sleep 0.05; done; fail "$what"; }
on_bus() { DBUS_SESSION_BUS_ADDRESS=$BUS gdbus introspect --session -d "$1" -o "$2" > /dev/null 2>&1; }
mapped() { [[ $(run "$ANDROMEDA" windows --json | jq "[.. | objects | select(.app_id? == \"$APP\")] | length") -gt 0 ]]; }
action() {
  DBUS_SESSION_BUS_ADDRESS=$BUS gdbus call --session -d "$APP" -o /dev/andromeda/Files \
    -m org.freedesktop.Application.ActivateAction "$1" "${2:-[]}" '{}' > /dev/null
}
is_mounted() { grep -q " $MOUNT fuse" /proc/self/mounts; }
not_mounted() { ! is_mounted; }
logged() { grep -q "$1" "$OUT/files.log"; }
wait_for "Files is not on the bus" on_bus "$APP" /dev/andromeda/Files
wait_for "the Files window did not map" mapped
run "$ANDROMEDA" settle > /dev/null

# New Vault: the name comes with the action, the password through sheet.password (headless keyboards drop keys).
action vault.new "[<'$NAME'>]"
run "$ANDROMEDA" settle > /dev/null
run grim "$OUT/new-vault.png"
action sheet.password "[<'$PASSWORD'>]"
wait_for "New Vault did not open the vault at $MOUNT" is_mounted
wait_for "the window did not go into the vault" logged "location $MOUNT"
echo "secret plans" > "$MOUNT/plan.txt"
cipher=$HOME_DIR/.local/share/andromeda/vaults/$NAME
[[ -f $cipher/gocryptfs.conf ]] || fail "no gocryptfs cipher folder at $cipher"
! grep -rq "secret plans" "$cipher" || fail "the cipher folder holds the plain content"
! find "$cipher" -name 'plan.txt' | grep -q . || fail "the cipher folder shows the plain file name"
echo "pass create"

wait_for "the guard took no sleep inhibitor" grep -q "INHIBIT sleep delay" "$OUT/logind.log"
action vault.lock "[<'$NAME'>]"
wait_for "Lock did not unmount the vault" not_mounted
[[ ! -e $MOUNT/plan.txt ]] || fail "the file shows after Lock"
echo "pass lock"

action vault.unlock "[<'$NAME'>]"
run "$ANDROMEDA" settle > /dev/null
action sheet.password "[<'wrong-password'>]"
wait_for "a wrong password gave no error" logged "vault wrong password $NAME"
not_mounted || fail "a wrong password unlocked the vault"
action sheet.password "[<'$PASSWORD'>]"
wait_for "Unlock did not mount the vault" is_mounted
[[ $(cat "$MOUNT/plan.txt") == "secret plans" ]] || fail "the file did not come back after Unlock"
echo "pass unlock"

inhibitors() { [[ $(grep -c "INHIBIT sleep delay" "$OUT/logind.log") -ge 2 ]]; }
wait_for "the guard took no sleep inhibitor after Unlock" inhibitors
echo sleep >&7
wait_for "the guard did not lock the vault before sleep" not_mounted
echo "pass lock before sleep"
run "$ANDROMEDA" settle > /dev/null
run grim "$OUT/locked.png"
echo "pass files_vault"
