#!/usr/bin/env bash
# Files v2 FB-POWER smart folders (mockup files-v2-live.html §16): saved searches as sidebar places.
# Fails when:
#   - "Kind is Image · Modified in the last 30 days · in This PC" does not save to smart-folders.json and the sidebar,
#     or its listing is not the one recent image,
#   - a new matching file does not show after Reload (the search runs again),
#   - a folder-scoped smart folder finds files outside its folder,
#   - a smart folder without a condition or with a taken name is accepted,
#   - Remove Smart Folder leaves it in the sidebar or the file.
# SCALE=2 writes the 2x sheet shot for the mockup pair. Shots and logs go to $OUT (default ./artifacts/files-smart).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/files-smart}
source "$(dirname "$0")/lib.sh"
FILES=${FILES:-$ROOT/apps/files/build/andromeda-files}
APP=dev.andromeda.Files
HOME_DIR=$RUNTIME/home
PICS=$HOME_DIR/Pictures
CONFIG=$HOME_DIR/.config/andromeda/files/smart-folders.json
boot_headless 1
if [[ ${SCALE:-1} == 2 ]]; then
  printf '\n[output.HEADLESS-1]\nmode = "2560x1440@60"\nscale = 2\n' >> "$RUNTIME/andromeda.toml"
  run "$ANDROMEDA" msg config-reload > /dev/null
  for _ in $(seq 100); do run "$ANDROMEDA" outputs --json | jq -e ".[0].scale == 2" > /dev/null && break; sleep 0.05; done
fi

fail() { echo "FAIL: $*"; exit 1; }
mkdir -p "$PICS" "$HOME_DIR/docs"
touch "$PICS/today.jpg" "$PICS/notes.txt" "$HOME_DIR/docs/notes-draft.txt" "$HOME_DIR/docs/report.pdf"
touch -d '2020-05-01' "$PICS/old.png"

bus_info=$(dbus-daemon --config-file="$ROOT/tests/e2e/private-bus.conf" --fork --print-address=1 --print-pid=1)
BUS=$(sed -n 1p <<< "$bus_info")
BUS_PID=$(sed -n 2p <<< "$bus_info")
trap 'kill "$BUS_PID" 2> /dev/null || true; kill $(jobs -p) 2> /dev/null || true; wait 2> /dev/null; rm_runtime' EXIT
env_app=(env DBUS_SESSION_BUS_ADDRESS="$BUS" HOME="$HOME_DIR" XDG_DATA_HOME="$HOME_DIR/.local/share"
  XDG_STATE_HOME="$HOME_DIR/.local/state" XDG_CONFIG_HOME="$HOME_DIR/.config")
run "${env_app[@]}" NOCTALIA_LOG_LEVEL=debug "$FILES" "$PICS" > "$OUT/files.log" 2>&1 &

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
count_logged() { grep -c -- "$1" "$OUT/files.log" || true; }
sidebar() { grep -o 'sidebar smart \[[^]]*\]' "$OUT/files.log" | tail -n 1; }

wait_for "the folder did not list" logged "location $PICS"
action file.new-smart-folder
action smart.set "[<['name','Recent Images']>]"
action smart.set "[<['kind','image']>]"
action smart.set "[<['days','30']>]"
action smart.set "[<['where','pc']>]"
run "$ANDROMEDA" settle > /dev/null
sleep 0.3
run grim "$OUT/smart-sheet.png"
action smart.save
wait_for "the smart folder did not save" \
  logged "smart folder saved Recent Images: Kind is Image · Modified in the last 30 days · in This PC"
wait_for "the listing is not the one recent image" logged "smart folder Recent Images: 1 results"
[[ $(jq -r '.[0].kind + " " + (.[0].days|tostring)' "$CONFIG") == "image 30" ]] || fail "smart-folders.json: $(cat "$CONFIG")"
wait_for "the sidebar does not show it: $(sidebar)" test "$(sidebar)" = "sidebar smart [Recent Images]"
echo "PASS save to sidebar"

touch "$PICS/new.png"
action view.reload
wait_for "Reload did not find new.png" logged "smart folder Recent Images: 2 results"
echo "PASS runs again"

action go "[<'$PICS'>]"
action file.new-smart-folder
action smart.set "[<['name','Pictures Notes']>]"
action smart.set "[<['text','notes']>]"
action smart.set "[<['where','here']>]"
action smart.save
wait_for "the folder scope found files outside Pictures" logged "smart folder Pictures Notes: 1 results"
echo "PASS folder scope"

before=$(count_logged "smart folder saved")
action file.new-smart-folder
action smart.set "[<['name','Recent Images']>]"
action smart.set "[<['text','x']>]"
action smart.save
action smart.set "[<['name','Empty']>]"
action smart.set "[<['text','']>]"
action smart.save
action smart.set "[<['name','Fine']>]"
action smart.set "[<['text','report']>]"
action smart.save
wait_for "the valid smart folder did not save" logged "smart folder saved Fine"
[[ $(count_logged "smart folder saved") -eq $((before + 1)) ]] || fail "a taken name or an empty search was saved"
echo "PASS bad criteria refused"

action smart.go "[<'Recent Images'>]"
action file.remove-smart-folder
wait_for "Remove left it in the sidebar: $(sidebar)" test "$(sidebar)" = "sidebar smart [Pictures Notes,Fine]"
[[ $(jq -r '[.[].name] | join(",")' "$CONFIG") == "Pictures Notes,Fine" ]] || fail "smart-folders.json keeps it"
echo "PASS remove"
