#!/usr/bin/env bash
# Files v2 FB-TAGS (mockup files-v2-live.html §17): the Tags sheet renames, orders, hides, recolours and deletes tags.
# Fails when:
#   - renaming Work to Projects leaves a file with "Work", drops its other tags or changes their order,
#     or tags.json keeps Work or moves the tag; the sidebar still lists Work,
#   - a name that is taken or holds a comma is accepted,
#   - Move Up does not swap Projects with the tag above it in tags.json,
#   - hiding Urgent keeps it in the sidebar, or the colour change does not reach tags.json,
#   - deleting Family leaves it on c.txt or in tags.json; Done does not close the sheet.
# SCALE=2 writes the 2x sheet shot for the mockup pair. Shots and logs go to $OUT (default ./artifacts/files-tags).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/files-tags}
source "$(dirname "$0")/lib.sh"
FILES=${FILES:-$ROOT/apps/files/build/andromeda-files}
FILEOPS=${FILEOPS:-$ROOT/system/target/debug/andromeda-fileops}
APP=dev.andromeda.Files
HOME_DIR=$RUNTIME/home
DOCS=$HOME_DIR/docs
CONFIG=$HOME_DIR/.config/andromeda/files/tags.json
boot_headless 1
if [[ ${SCALE:-1} == 2 ]]; then
  printf '\n[output.HEADLESS-1]\nmode = "2560x1440@60"\nscale = 2\n' >> "$RUNTIME/andromeda.toml"
  run "$ANDROMEDA" msg config-reload > /dev/null
  for _ in $(seq 100); do run "$ANDROMEDA" outputs --json | jq -e ".[0].scale == 2" > /dev/null && break; sleep 0.05; done
fi

fail() { echo "FAIL: $*"; exit 1; }
mkdir -p "$DOCS"
for f in a b c d; do echo "$f" > "$DOCS/$f.txt"; done
setfattr -n user.xdg.tags -v "Work" "$DOCS/a.txt" || fail "this file system takes no user xattrs"
setfattr -n user.xdg.tags -v "Urgent,Work,Ideas" "$DOCS/b.txt"
setfattr -n user.xdg.tags -v "Family" "$DOCS/c.txt"

bus_info=$(dbus-daemon --session --fork --print-address=1 --print-pid=1)
BUS=$(sed -n 1p <<< "$bus_info")
BUS_PID=$(sed -n 2p <<< "$bus_info")
trap 'kill "$BUS_PID" 2> /dev/null || true; kill $(jobs -p) 2> /dev/null || true; wait 2> /dev/null; rm_runtime' EXIT
env_app=(env DBUS_SESSION_BUS_ADDRESS="$BUS" HOME="$HOME_DIR" XDG_DATA_HOME="$HOME_DIR/.local/share"
  XDG_STATE_HOME="$HOME_DIR/.local/state" XDG_CONFIG_HOME="$HOME_DIR/.config")
run "${env_app[@]}" XDG_RUNTIME_DIR="$RUNTIME" "$FILEOPS" > "$OUT/fileops.log" 2>&1 &
run "${env_app[@]}" NOCTALIA_LOG_LEVEL=debug "$FILES" "$DOCS" > "$OUT/files.log" 2>&1 &

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
seen() { [[ $(grep -c -- "$1" "$OUT/files.log") -ge $2 ]]; } # the log holds $1 at least $2 times
wait_for() { # description, test command...
  local what=$1; shift
  for _ in $(seq 100); do "$@" && return 0; sleep 0.05; done
  fail "$what"
}
tags_of() { getfattr --only-values -n user.xdg.tags "$1" 2> /dev/null || true; }
has_tags() { [[ $(tags_of "$1") == "$2" ]]; }
config_names() { jq -r '[.[].name] | join(",")' "$CONFIG" 2> /dev/null; }
config_is() { [[ $(config_names) == "$1" ]]; }
sidebar() { grep -o 'sidebar tags \[[^]]*\]' "$OUT/files.log" | tail -n 1; }
sidebar_is() { [[ $(sidebar) == "sidebar tags [$1]" ]]; }

wait_for "the daemon is not on the bus" \
  env DBUS_SESSION_BUS_ADDRESS="$BUS" gdbus introspect --session -d org.andromeda.FileOps1 -o /org/andromeda/FileOps1

action file.tags
wait_for "Edit Tags… did not open the sheet" grep -q "tag manager open" "$OUT/files.log"
wait_for "the sheet did not count the tagged files" grep -q "sidebar tags \[Urgent,Personal,Ideas,Done,Work,Family\]" "$OUT/files.log"
action tags.select "[<['Work']>]"
run "$ANDROMEDA" settle > /dev/null
sleep 0.5 # the sheet fade-in; settle does not wait for it
run grim "$OUT/tags-sheet.png"

action tags.rename "[<['Work','Projects']>]"
wait_for "a.txt did not get Projects" has_tags "$DOCS/a.txt" "Projects"
wait_for "b.txt lost its order or other tags: $(tags_of "$DOCS/b.txt")" has_tags "$DOCS/b.txt" "Urgent,Projects,Ideas"
config_is "Urgent,Personal,Ideas,Done,Projects,Family" || fail "tags.json after the rename: $(config_names)"
wait_for "the sidebar did not follow the rename: $(sidebar)" sidebar_is "Urgent,Personal,Ideas,Done,Projects,Family"
echo "PASS rename keeps the tag on its files"

action tags.rename "[<['Projects','Urgent']>]"
action tags.rename "[<['Projects','A,B']>]"
wait_for "the taken name was not refused" grep -q "tag rename refused: There is already a tag named" "$OUT/files.log"
wait_for "the comma name was not refused" grep -q "tag rename refused: A tag name can’t" "$OUT/files.log"
config_is "Urgent,Personal,Ideas,Done,Projects,Family" || fail "a refused rename changed tags.json: $(config_names)"
echo "PASS bad names refused"

action tags.move "[<['Projects','up']>]"
wait_for "Move Up did not reorder tags.json: $(config_names)" config_is "Urgent,Personal,Ideas,Projects,Done,Family"
wait_for "the sidebar did not follow the order: $(sidebar)" sidebar_is "Urgent,Personal,Ideas,Projects,Done,Family"
echo "PASS reorder"

action tags.sidebar "[<['Urgent','off']>]"
wait_for "Urgent still shows in the sidebar: $(sidebar)" sidebar_is "Personal,Ideas,Projects,Done,Family"
[[ $(jq -r '.[] | select(.name == "Urgent") | .sidebar' "$CONFIG") == false ]] || fail "tags.json keeps Urgent shown"
has_tags "$DOCS/b.txt" "Urgent,Projects,Ideas" || fail "hiding Urgent changed the files"
action tags.color "[<['Projects','#8a4fd8']>]"
wait_for "the colour did not reach tags.json" \
  test "$(jq -r '.[] | select(.name == "Projects") | .color' "$CONFIG")" = "#8a4fd8"
echo "PASS hide from sidebar and colour"

action tags.delete "[<['Family']>]"
wait_for "Delete left Family on c.txt: $(tags_of "$DOCS/c.txt")" has_tags "$DOCS/c.txt" ""
config_is "Urgent,Personal,Ideas,Projects,Done" || fail "tags.json keeps Family: $(config_names)"
echo "PASS delete"

action tags.new
wait_for "New Tag did not add a tag" config_is "Urgent,Personal,Ideas,Projects,Done,New Tag"
run "$ANDROMEDA" settle > /dev/null
sleep 0.5
run grim "$OUT/tags-sheet-new.png"
action tags.done
# Commands wait while a sheet shows: the second open proves Done closed it.
action file.tags
wait_for "the sheet did not open again after Done" seen 'tag manager open' 2
echo "PASS done"
