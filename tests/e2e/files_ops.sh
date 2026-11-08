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
# SCALE=2: a 2560x1440 mode at scale 2, for 1:1 crops against the mockup at device scale 2.
if [[ ${SCALE:-1} == 2 ]]; then
  printf '\n[output.HEADLESS-1]\nmode = "2560x1440@60"\nscale = 2\n' >> "$RUNTIME/andromeda.toml"
  # The compositor read its config at boot: reload, and wait until the output runs at scale 2.
  run "$ANDROMEDA" msg config-reload > /dev/null
  for _ in $(seq 100); do run "$ANDROMEDA" outputs --json | jq -e ".[0].scale == 2" > /dev/null && break; sleep 0.05; done
  run "$ANDROMEDA" outputs --json | jq -e ".[0].scale == 2" > /dev/null || { echo "FAIL: the output did not switch to scale 2"; exit 1; }
fi

fail() { echo "FAIL: $*"; exit 1; }
mkdir -p "$SRC" "$DST"
echo new > "$SRC/a.txt"
echo old > "$DST/a.txt"

bus_info=$(dbus-daemon --config-file="$ROOT/tests/e2e/private-bus.conf" --fork --print-address=1 --print-pid=1)
BUS=$(sed -n 1p <<< "$bus_info")
BUS_PID=$(sed -n 2p <<< "$bus_info")
trap 'kill "$BUS_PID" 2> /dev/null || true; kill $(jobs -p) 2> /dev/null || true; wait 2> /dev/null; rm_runtime' EXIT
env_app=(env DBUS_SESSION_BUS_ADDRESS="$BUS" HOME="$HOME_DIR" XDG_DATA_HOME="$HOME_DIR/.local/share"
  XDG_STATE_HOME="$HOME_DIR/.local/state" XDG_CONFIG_HOME="$HOME_DIR/.config")

# The daemon gets 3 % of a CPU, so the 1 GB copy of the job pill step runs for seconds and emits progress while the
# test clicks; the small steps before it do not notice.
systemd-run --user --scope --quiet -p CPUQuota=3% "${env_app[@]}" XDG_RUNTIME_DIR="$RUNTIME" "$FILEOPS" \
  > "$OUT/fileops.log" 2>&1 &
run "${env_app[@]}" NOCTALIA_LOG_LEVEL=debug DESK_FILES_GEOMETRY=1 "$FILES" "$SRC" > "$OUT/files.log" 2>&1 &

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
    grep -q "selected $2 of .* in $1" < <(tail -n +"$((before + 1))" "$OUT/files.log") && return 0
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
# The app hears the conflict a moment after the daemon: Keep Both is enabled only then.
wait_for "the window did not show the conflict" grep -q "job copy conflict" "$OUT/files.log"
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

# The system clipboard: Copy offers the files to other apps, Copy Location offers the path as text, and Paste takes
# files another app copied (wl-copy stands in for it). Real keys: the compositor takes a clipboard only with the
# serial of an input event.
POINTER=${POINTER:-$ROOT/compositor/build-debug/tests/pointer-client}
keys() { run "$POINTER" 1280 720 keyboard-only tap 119 pause 100 "$@" > /dev/null; }
action go "[<'$DST'>]"
select_names "$DST" 1 "['a.txt']"
keys mod control tap 46 mod none                 # Ctrl+C (KEY_C)
clip_has() { [[ $(run wl-paste -n -t "$1" 2> /dev/null) == "$2" ]]; }
wait_for "Copy did not put the file on the clipboard" clip_has text/uri-list "$(printf 'file://%s/a.txt\r' "$DST")"
wait_for "Copy did not offer GNOME's copied-files type" clip_has x-special/gnome-copied-files "$(printf 'copy\nfile://%s/a.txt' "$DST")"
keys mod control+alt tap 46 mod none             # Ctrl+Alt+C: Copy Location
wait_for "Copy Location did not put the path on the clipboard" clip_has text/plain "$DST/a.txt"
echo outside > "$HOME_DIR/outside.txt"
run wl-copy -t text/uri-list "file://$HOME_DIR/outside.txt"
action go "[<'$SRC'>]"
keys mod control tap 47 mod none                 # Ctrl+V (KEY_V)
wait_for "Paste did not copy the file another app put on the clipboard" test -f "$SRC/outside.txt"
[[ $(cat "$SRC/outside.txt") == outside ]] || fail "the pasted file has other content"
run wl-copy --clear # ends the wl-copy daemon, which would outlive the test
echo "PASS clipboard"

# Job pill (FILES-UI-SPEC §14): a slow copy (the daemon runs at 3 % CPU) shows the header pill; a click opens the
# popover; while progress arrives, its Pause circle pauses the copy, Resume runs it again, and Stop cancels it.
# Real data: the copy skips the holes of a sparse file and ends before the pill shows.
head -c 512M /dev/zero > "$SRC/big.bin"
select_names "$SRC" 1 "['big.bin']"
action edit.copy
action go "[<'$DST'>]"
action edit.paste
wait_for "the pill did not show for the running copy" grep -q "job pill 1" "$OUT/files.log"
job=$(grep -oE "job copy running /org/andromeda/FileOps1/Job/[0-9]+" "$OUT/files.log" | tail -n 1 | awk '{print $NF}')
[[ -n $job ]] || fail "the copy logged no running state"
run "$ANDROMEDA" settle > /dev/null
wait_for "the pill logged no geometry" grep -q "geometry pill " "$OUT/files.log"
read -r px py pw ph <<< "$(grep -E "geometry pill " "$OUT/files.log" | tail -n 1 | awk '{print $(NF-3), $(NF-2), $(NF-1), $NF}')"
WX=$(jq .x <<< "$w")
WY=$(jq .y <<< "$w")
run "$POINTER" 1280 720 move $((WX + px + pw / 2)) $((WY + py + ph / 2)) click 272 > /dev/null
wait_for "a click on the pill did not open the popover" grep -q "job popover open" "$OUT/files.log"
run "$ANDROMEDA" settle > /dev/null
run grim "$OUT/pill-popover.png"
# The card hangs under the pill, right edges lined up. First row: Stop 12 in from the right, Pause 8 left of it.
stop_x=$((WX + px + pw - 24))
pause_x=$((WX + px + pw - 56))
row_y=$((WY + py + ph + 61))
progress_after() { grep -q "job copy running $job" < <(tail -n +"$(($1 + 1))" "$OUT/files.log"); }
m=$(wc -l < "$OUT/files.log")
wait_for "no progress arrived while the popover was open" progress_after "$m"
run "$POINTER" 1280 720 move "$pause_x" "$row_y" click 272 > /dev/null
wait_for "the Pause circle did not pause the copy" grep -q "job pause $job" "$OUT/files.log"
wait_for "the copy did not reach paused" grep -q "job copy paused $job" "$OUT/files.log"
run "$POINTER" 1280 720 move "$pause_x" "$row_y" click 272 > /dev/null
wait_for "the Resume circle did not resume the copy" grep -q "job resume $job" "$OUT/files.log"
m=$(wc -l < "$OUT/files.log")
wait_for "no progress arrived after Resume" progress_after "$m"
run "$POINTER" 1280 720 move "$stop_x" "$row_y" click 272 > /dev/null
wait_for "the Stop circle did not cancel the copy" grep -q "job cancel $job" "$OUT/files.log"
wait_for "the copy did not end cancelled" grep -q "job copy cancelled" "$OUT/files.log"
wait_for "the pill did not go when the copy ended" grep -q "job pill 0" "$OUT/files.log"
echo "PASS job pill"
