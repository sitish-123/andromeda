#!/usr/bin/env bash
# Files P2: copy with a conflict, undo, trash and restore through andromeda-fileops (docs/apps.md §3).
# The app and the daemon share a private session bus; the script drives the app through
# org.freedesktop.Application.ActivateAction (the same command ids as the keyboard shortcuts). Fails when:
#   - pasting a.txt into a folder that holds a.txt does not raise a conflict, or Keep Both does not add "a (copy).txt",
#   - Undo does not remove the copy, or leaves the original changed,
#   - Move to Trash does not move a.txt into the home Trash, or Put Back does not return it with its content.
# Writes the app and daemon logs to $OUT (default ./artifacts/files-ops).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/files-ops}
source "$(dirname "$0")/lib.sh"
FILES=${FILES:-$ROOT/apps/files/build/andromeda-files}
FILEOPS=${FILEOPS:-$ROOT/system/target/debug/andromeda-fileops}
APP=dev.andromeda.Files
HOME_DIR=$RUNTIME/home
SRC=$HOME_DIR/src
DST=$HOME_DIR/dst
TRASH=$HOME_DIR/.local/share/Trash
boot_headless 1

fail() { echo "FAIL: $*"; exit 1; }
mkdir -p "$SRC" "$DST"
echo new > "$SRC/a.txt"
echo old > "$DST/a.txt"

bus_info=$(dbus-daemon --session --fork --print-address=1 --print-pid=1)
BUS=$(sed -n 1p <<< "$bus_info")
BUS_PID=$(sed -n 2p <<< "$bus_info")
trap 'kill "$BUS_PID" 2> /dev/null || true; kill $(jobs -p) 2> /dev/null || true; wait 2> /dev/null; rm -rf "$RUNTIME"' EXIT
env_app=(env DBUS_SESSION_BUS_ADDRESS="$BUS" HOME="$HOME_DIR" XDG_DATA_HOME="$HOME_DIR/.local/share"
  XDG_STATE_HOME="$HOME_DIR/.local/state" XDG_CONFIG_HOME="$HOME_DIR/.config")

run "${env_app[@]}" "$FILEOPS" > "$OUT/fileops.log" 2>&1 &
run "${env_app[@]}" NOCTALIA_LOG_LEVEL=debug "$FILES" "$SRC" > "$OUT/files.log" 2>&1 &

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
# The listing is async: select again until the app logs the selection with the expected count.
select_names() { # where-label, count, names gvariant
  local before
  for _ in $(seq 40); do
    before=$(wc -l < "$OUT/files.log")
    action select "[<$3>]"
    sleep 0.05
    tail -n +"$((before + 1))" "$OUT/files.log" | grep -q "selected $2 of .* in $1" && return 0
  done
  fail "could not select $3 in $1"
}
gone() { [[ ! -e $1 ]]; }
in_conflict() { fileops_state | grep -q conflict; }
count() { find "$1" -mindepth 1 -maxdepth 1 | wc -l; }
fileops_state() { # last job's State
  DBUS_SESSION_BUS_ADDRESS=$BUS gdbus introspect --session -d org.andromeda.FileOps1 -o /org/andromeda/FileOps1 -r -p \
    | grep -o "State = '[a-z]*'" | tail -n 1
}

wait_for "the daemon is not on the bus" \
  env DBUS_SESSION_BUS_ADDRESS="$BUS" gdbus introspect --session -d org.andromeda.FileOps1 -o /org/andromeda/FileOps1

select_names "$SRC" 1 "['a.txt']"
action edit.copy
action go "[<'$DST'>]"
action edit.paste
wait_for "pasting a.txt into dst raised no conflict" in_conflict
action conflict.keep-both
wait_for "Keep Both did not add 'a (copy).txt'" test -f "$DST/a (copy).txt"
[[ $(cat "$DST/a (copy).txt") == new && $(cat "$DST/a.txt") == old ]] || fail "Keep Both changed the wrong file"
echo "PASS copy with conflict (Keep Both)"

wait_for "the copy job did not finish as undoable" grep -q "job copy done undoable" "$OUT/files.log"
action edit.undo
wait_for "Undo did not remove 'a (copy).txt'" gone "$DST/a (copy).txt"
[[ $(cat "$DST/a.txt") == old && $(count "$DST") -eq 1 ]] || fail "Undo left dst as: $(ls "$DST")"
echo "PASS undo"

select_names "$DST" 1 "['a.txt']"
action edit.trash
wait_for "Move to Trash did not remove dst/a.txt" gone "$DST/a.txt"
wait_for "a.txt is not in the home Trash" test -f "$TRASH/info/a.txt.trashinfo"
echo "PASS trash"

action trash
select_names trash 1 "['a.txt']"
action file.restore
wait_for "Put Back did not return dst/a.txt" test -f "$DST/a.txt"
[[ $(cat "$DST/a.txt") == old ]] || fail "Put Back returned different content"
[[ ! -e $TRASH/info/a.txt.trashinfo ]] || fail "the trashinfo file is still there"
echo "PASS restore"
