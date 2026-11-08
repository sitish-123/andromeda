#!/usr/bin/env bash
# Files v2 FB3 search (FILES-UI-SPEC §15): Ctrl+F and typing filter the folder; the This PC chip asks andromeda-index
# and lists hits from the whole home with a Where column; kind chips narrow both scopes.
# Fails when:
#   - typing in the search field does not filter the folder to its one match,
#   - the chips row does not show while searching, or This PC is missing with the index running,
#   - This PC does not list the hits outside the folder, or the PDFs chip keeps non-PDF hits,
#   - This PC results do not show as a list, or This Folder does not bring the icons view back,
#   - a query with no hits does not show the no-results state,
#   - Recents does not add the files the index saw change in the last 30 days.
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/files-search}
source "$(dirname "$0")/lib.sh"
FILES=${FILES:-$ROOT/apps/files/build/andromeda-files}
INDEX=${INDEX:-$ROOT/indexer/build/andromeda-index}
POINTER=$ROOT/compositor/build-debug/tests/pointer-client
APP=dev.andromeda.Files
HOME_DIR=$RUNTIME/home
SRC=$HOME_DIR/Work
boot_headless 1
mkdir -p "$OUT"

fail() { echo "FAIL: $*"; exit 1; }
mkdir -p "$SRC" "$HOME_DIR/Documents" "$HOME_DIR/Pictures" "$HOME_DIR/.local/state"
echo draft > "$SRC/invoice-draft.txt"
echo other > "$SRC/notes.txt"
printf '%%PDF-1.4\n%%%%EOF\n' > "$HOME_DIR/Documents/invoice-march.pdf"
echo text > "$HOME_DIR/Documents/invoice-notes.txt"
printf '\x89PNG\r\n\x1a\n' > "$HOME_DIR/Pictures/invoice-scan.png"

bus_info=$(dbus-daemon --session --fork --print-address=1 --print-pid=1)
BUS=$(sed -n 1p <<< "$bus_info")
BUS_PID=$(sed -n 2p <<< "$bus_info")
trap 'kill "$BUS_PID" 2> /dev/null || true; kill $(jobs -p) 2> /dev/null || true; wait 2> /dev/null; rm_runtime' EXIT
env XDG_RUNTIME_DIR="$RUNTIME" HOME="$HOME_DIR" XDG_STATE_HOME="$HOME_DIR/.local/state" "$INDEX" serve \
  > "$OUT/index.log" 2>&1 &
for _ in $(seq 200); do grep -q watching "$OUT/index.log" && break; sleep 0.05; done
grep -q watching "$OUT/index.log" || fail "the index daemon did not start"

run env DBUS_SESSION_BUS_ADDRESS="$BUS" HOME="$HOME_DIR" XDG_STATE_HOME="$HOME_DIR/.local/state" \
  XDG_CONFIG_HOME="$HOME_DIR/.config" LC_ALL=C.UTF-8 NOCTALIA_LOG_LEVEL=debug DESK_MOTION_TRACE=1 DESK_FILES_GEOMETRY=1 \
  "$FILES" "$SRC" > "$OUT/files.log" 2>&1 &

win() { run "$ANDROMEDA" windows --json | jq -c "[.. | objects | select(.app_id? == \"$APP\")][0] // empty"; }
for _ in $(seq 100); do
  w=$(win)
  [[ -n $w && $(jq .w <<< "$w") -gt 0 ]] && break
  sleep 0.05
done
[[ -n ${w:-} ]] || fail "the Files window did not map"
WX=$(jq .x <<< "$w")
WY=$(jq .y <<< "$w")
run "$ANDROMEDA" settle > /dev/null

wait_log() {
  for _ in $(seq 60); do
    tail -n +"$(($1 + 1))" "$OUT/files.log" | grep -qE "$2" && return 0
    sleep 0.05
  done
  return 1
}
mark() { wc -l < "$OUT/files.log"; }
keys() { run "$POINTER" 1280 720 keyboard-only tap 119 pause 100 "$@" > /dev/null; }
ptr() { run "$POINTER" 1280 720 "$@" > /dev/null; }
shot() { run "$ANDROMEDA" settle > /dev/null; run grim "$OUT/$1.png"; }
at() {
  local line
  line=$(grep -E "geometry $1 -?[0-9]+ -?[0-9]+$" "$OUT/files.log" | tail -n 1)
  [[ -n $line ]] || fail "no geometry for $1"
  read -r x y <<< "$(awk '{print $(NF-1), $NF}' <<< "$line")"
  echo "$((WX + x)) $((WY + y))"
}
# invoice = i n v o i c e
INVOICE=(tap 23 tap 49 tap 47 tap 24 tap 23 tap 46 tap 18)

wait_log 0 "location $SRC" || fail "the window did not list $SRC"
m=$(mark); keys mod control tap 33 mod none "${INVOICE[@]}"     # Ctrl+F, "invoice"
wait_log "$m" 'filter "invoice" shows 1' || fail "the folder filter did not narrow to invoice-draft.txt"
wait_log "$m" "geometry chip This PC" || fail "the chips row or its This PC chip did not show"
shot 1-folder

m=$(mark); ptr move $(at "chip This PC") click 272
wait_log "$m" "search scope pc" || fail "the This PC chip did not switch the scope"
wait_log "$m" "search results [4-9]" || fail "the index did not answer with the home hits"
wait_log "$m" "view list" || fail "This PC results did not switch to the list with the Where column"
wait_log "$m" "settle files\.view" || fail "the switch to the list did not settle"
wait_log "$m" "geometry item invoice-march.pdf" || fail "This PC did not list invoice-march.pdf"
shot 2-this-pc

m=$(mark); ptr move $(at "chip PDFs") click 272
wait_log "$m" "search kind pdf" || fail "the PDFs chip did not set the kind"
wait_log "$m" "search results 1$" || fail "PDFs kept other kinds"
shot 3-pdfs

m=$(mark); ptr move $(at "chip All") click 272
keys mod control tap 33 mod none tap 44 tap 44 tap 44              # Ctrl+F, "zzz" (or "invoicezzz"): no hits
wait_log "$m" 'search results 0$' || fail "a query with no hits did not answer empty"
shot 4-no-results
m=$(mark); ptr move $(at "chip This Folder") click 272
wait_log "$m" "search scope folder" || fail "the This Folder chip did not switch the scope back"
wait_log "$m" "view icons" || fail "leaving This PC did not bring the folder's icons view back"
# Recents: no app reported these files (no recently-used.xbel); the index adds the ones changed in the last 30 days.
m=$(mark); ptr move $(at "place Recents") click 272
wait_log "$m" "recents [3-9] rows" || fail "Recents did not add the index's recent files"
wait_log "$m" "geometry item invoice-march.pdf" || fail "Recents did not list invoice-march.pdf"
shot 5-recents
echo "pass files_search"
