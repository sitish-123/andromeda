#!/usr/bin/env bash
# Files v2 FB-POWER Select by Pattern (Ctrl+S, mockup files-v2-live.html §16).
# Fails when:
#   - "*.jpg | *.png" does not select the 4 images (case-insensitive) of 7 items,
#   - Deselect with "*.png" does not take the 2 PNGs out of that selection,
#   - Regex "^IMG_\d+" does not add the IMG files to the selection,
#   - a broken regex is accepted.
# SCALE=2 writes the 2x sheet shot for the mockup pair. Shots and logs go to $OUT (default ./artifacts/files-pattern).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/files-pattern}
source "$(dirname "$0")/lib.sh"
FILES=${FILES:-$ROOT/apps/files/build/andromeda-files}
APP=dev.andromeda.Files
HOME_DIR=$RUNTIME/home
DIR=$HOME_DIR/photos
boot_headless 1
if [[ ${SCALE:-1} == 2 ]]; then
  printf '\n[output.HEADLESS-1]\nmode = "2560x1440@60"\nscale = 2\n' >> "$RUNTIME/andromeda.toml"
  run "$ANDROMEDA" msg config-reload > /dev/null
  for _ in $(seq 100); do run "$ANDROMEDA" outputs --json | jq -e ".[0].scale == 2" > /dev/null && break; sleep 0.05; done
fi

fail() { echo "FAIL: $*"; exit 1; }
mkdir -p "$DIR"
touch "$DIR"/{IMG_0412.jpg,IMG_0413.JPG,shot.png,logo.PNG,notes.txt,IMG_draft.txt,readme.md}

bus_info=$(dbus-daemon --config-file="$ROOT/tests/e2e/private-bus.conf" --fork --print-address=1 --print-pid=1)
BUS=$(sed -n 1p <<< "$bus_info")
BUS_PID=$(sed -n 2p <<< "$bus_info")
trap 'kill "$BUS_PID" 2> /dev/null || true; kill $(jobs -p) 2> /dev/null || true; wait 2> /dev/null; rm_runtime' EXIT
env_app=(env DBUS_SESSION_BUS_ADDRESS="$BUS" HOME="$HOME_DIR" XDG_DATA_HOME="$HOME_DIR/.local/share"
  XDG_STATE_HOME="$HOME_DIR/.local/state" XDG_CONFIG_HOME="$HOME_DIR/.config")
run "${env_app[@]}" NOCTALIA_LOG_LEVEL=debug "$FILES" "$DIR" > "$OUT/files.log" 2>&1 &

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
logged() { grep -q -- "$1" "$OUT/files.log"; }
last_selected() { grep -o "selected [0-9]* of [0-9]* in $DIR" "$OUT/files.log" | tail -n 1; }
selected_is() { [[ $(last_selected) == "selected $1 of $2 in $DIR" ]]; }

wait_for "the folder did not list" logged "location $DIR"
for _ in $(seq 40); do action edit.select-pattern; logged "pattern sheet" && break; sleep 0.05; done
action pattern.text "[<'*.jpg | *.png'>]"
run "$ANDROMEDA" settle > /dev/null
sleep 0.3
run grim "$OUT/pattern-sheet.png"
action pattern.select
wait_for "the wildcard did not select the 4 images: $(last_selected)" selected_is 4 4
echo "PASS wildcard select"

action edit.select-pattern
action pattern.text "[<'*.png'>]"
action pattern.deselect
wait_for "Deselect did not keep only the 2 JPGs: $(last_selected)" selected_is 2 2
echo "PASS deselect"

action edit.select-pattern
action pattern.mode "[<'regex'>]"
action pattern.text "[<'^IMG_\\\\d+'>]"
action pattern.select
wait_for "the regex did not select the IMG files: $(last_selected)" selected_is 2 2
action edit.select-pattern
action pattern.text "[<'IMG_draft'>]"
action pattern.select
wait_for "the regex did not add IMG_draft.txt: $(last_selected)" selected_is 3 3
echo "PASS regex select"

action edit.select-pattern
action pattern.text "[<'(['>]"
action pattern.select
wait_for "the broken regex was not refused" logged "pattern refused: This regular expression is not valid."
echo "PASS broken regex refused"
