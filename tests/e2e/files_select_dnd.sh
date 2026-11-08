#!/usr/bin/env bash
# Files v2 FB1 (FILES-UX.md §2.3, §6): pointer selection and drag and drop in the Files window over wl_data_device.
# Fails when:
#   - a click, Ctrl+click, Shift+click or a rubber band does not give the selection count it should,
#   - dragging 2 files onto a folder does not move them, Ctrl does not copy, Ctrl+Shift does not link,
#   - a drop on the sidebar Trash row does not trash the file,
#   - a drop onto the dragged folder itself is accepted,
#   - hovering a folder 700 ms does not spring-load into it, or the view does not return after the drop (D15),
#   - F3 does not open or close the split view, a press does not move the active pane, or a drag between panes does
#     not move the file.
# Shots go to $OUT (default ./artifacts/files-select-dnd).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/files-select-dnd}
source "$(dirname "$0")/lib.sh"
FILES=${FILES:-$ROOT/apps/files/build/andromeda-files}
FILEOPS=${FILEOPS:-$ROOT/system/target/debug/andromeda-fileops}
POINTER=$ROOT/compositor/build-debug/tests/pointer-client
APP=dev.andromeda.Files
HOME_DIR=$RUNTIME/home
SRC=$HOME_DIR/Work
boot_headless 1
mkdir -p "$OUT"

fail() { echo "FAIL: $*"; exit 1; }
mkdir -p "$SRC/Archive" "$SRC/Box/deep"
for f in alpha.txt beta.txt gamma.txt delta.txt omega.txt zeta.txt; do echo "$f" > "$SRC/$f"; done

bus_info=$(dbus-daemon --session --fork --print-address=1 --print-pid=1)
BUS=$(sed -n 1p <<< "$bus_info")
BUS_PID=$(sed -n 2p <<< "$bus_info")
trap 'kill "$BUS_PID" 2> /dev/null || true; kill $(jobs -p) 2> /dev/null || true; wait 2> /dev/null; rm_runtime' EXIT
ENVS=(DBUS_SESSION_BUS_ADDRESS="$BUS" HOME="$HOME_DIR" XDG_DATA_HOME="$HOME_DIR/.local/share"
  XDG_STATE_HOME="$HOME_DIR/.local/state" XDG_CONFIG_HOME="$HOME_DIR/.config" LC_ALL=C.UTF-8)
run env "${ENVS[@]}" "$FILEOPS" > "$OUT/fileops.log" 2>&1 &
run env "${ENVS[@]}" NOCTALIA_LOG_LEVEL=debug DESK_FILES_GEOMETRY=1 "$FILES" "$SRC" > "$OUT/files.log" 2>&1 &

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
  for _ in $(seq 80); do
    grep -qE "$2" < <(tail -n +"$(($1 + 1))" "$OUT/files.log") && return 0
    sleep 0.05
  done
  return 1
}
wait_file() {
  for _ in $(seq 80); do
    eval "$1" && return 0
    sleep 0.05
  done
  return 1
}
mark() { wc -l < "$OUT/files.log"; }
# The listing after a change: geometry line $3 right after $2 (rows shift when the watcher refreshes the folder).
wait_rows() {
  for _ in $(seq 80); do
    tail -n +"$(($1 + 1))" "$OUT/files.log" | grep -oE "geometry item [^ ]+" \
      | awk -v a="geometry $2" -v b="geometry $3" 'prev == a && $0 == b { found = 1 } { prev = $0 } END { exit !found }' \
      && return 0
    sleep 0.05
  done
  return 1
}
shot() { run "$ANDROMEDA" settle > /dev/null; run grim "$OUT/$1.png"; }
# Screen point of a geometry entry: "item alpha.txt", "place Trash", "view".
at() {
  local line
  line=$(grep -E "geometry $1( |$)" "$OUT/files.log" | tail -n 1)
  [[ -n $line ]] || fail "no geometry for $1"
  read -r x y <<< "$(awk '{print $(NF-1), $NF}' <<< "$line")"
  echo "$((WX + x)) $((WY + y))"
}
ptr() { run "$POINTER" 1280 720 "$@" > /dev/null; }
# Pause first: the first key of a new virtual keyboard only gives it focus.
ptrk() { run "$POINTER" 1280 720 tap 119 pause 100 "$@" > /dev/null; }
# A drag in steps past the start threshold, a rest on the target, then the drop.
drag() {
  local from=($1) to=($2) mods=${3:-none} rest=${4:-150}
  # The pause keeps a select click just before out of the 400 ms double click, which would open the file.
  ptrk mod "$mods" move "${from[0]}" "${from[1]}" pause 450 press 272 \
    move $((from[0] + 6)) $((from[1] + 6)) move $((from[0] + 20)) $((from[1] + 20)) \
    move $(((from[0] + to[0]) / 2)) $(((from[1] + to[1]) / 2)) move "${to[0]}" "${to[1]}" \
    pause "$rest" move $((to[0] + 1)) "${to[1]}" pause 50 release 272 mod none
}

wait_log 0 "geometry item omega.txt" || fail "no geometry for the listing"
wait_log 0 "first frame" || fail "the window drew no first frame"
shot 1-start

m=$(mark); ptr move $(at "item alpha.txt") click 272
wait_log "$m" "cursor alpha.txt selected 1" || fail "a click did not select alpha.txt"
m=$(mark); ptrk mod control move $(at "item beta.txt") click 272 mod none
wait_log "$m" "cursor beta.txt selected 2" || fail "Ctrl+click did not add beta.txt"
m=$(mark); ptrk mod shift move $(at "item omega.txt") click 272 mod none
wait_log "$m" "cursor omega.txt selected [0-9]+" || fail "Shift+click did not extend to omega.txt"

read -r vx vy <<< "$(at view)"
read -r ax ay <<< "$(at "item Archive")"
m=$(mark); ptr move "$vx" "$vy" click 272
wait_log "$m" "selected 0|0 of" || true
m=$(mark); ptr move $((vx + 200)) "$vy" press 272 move $((vx + 150)) $((vy - 40)) move $((ax - 30)) $((ay - 30)) \
  release 272
wait_log "$m" "band selected [1-9]" || fail "the rubber band selected nothing"
shot 2-band

# Two files onto a folder: same file system = move.
m=$(mark); ptr move $(at "item alpha.txt") click 272
ptrk mod control move $(at "item beta.txt") click 272 mod none
wait_log "$m" "selected 2" || fail "could not select alpha and beta"
m=$(mark); drag "$(at "item alpha.txt")" "$(at "item Archive")"
wait_log "$m" "drag 2 items" || fail "the drag did not start"
wait_log "$m" "drop target $SRC/Archive" || fail "Archive was not a drop target"
wait_file '[[ -f $SRC/Archive/alpha.txt && -f $SRC/Archive/beta.txt && ! -e $SRC/alpha.txt ]]' \
  || fail "dropping on Archive did not move alpha and beta"
wait_rows "$m" "item Box" "item delta.txt" || fail "the listing did not drop alpha and beta"

# Ctrl = copy.
m=$(mark); ptr move $(at "item gamma.txt") click 272
wait_log "$m" "cursor gamma.txt selected 1" || fail "could not select gamma"
drag "$(at "item gamma.txt")" "$(at "item Box")" control
wait_file '[[ -f $SRC/Box/gamma.txt && -f $SRC/gamma.txt ]]' || fail "Ctrl+drop did not copy gamma"

# Ctrl+Shift = link.
m=$(mark); ptr move $(at "item delta.txt") click 272
wait_log "$m" "cursor delta.txt selected 1" || fail "could not select delta"
drag "$(at "item delta.txt")" "$(at "item Box")" control+shift
wait_file '[[ -L $SRC/Box/delta.txt && -f $SRC/delta.txt ]]' || fail "Ctrl+Shift+drop did not link delta"

# A folder onto itself: no drop.
m=$(mark); ptr move $(at "item Box") click 272
drag "$(at "item Box")" "$(at "item Box")"
wait_log "$m" "drag 1 items" || fail "the Box drag did not start"
grep -qE "drop [0-9]+ items" < <(tail -n +"$((m + 1))" "$OUT/files.log") && fail "Box was dropped into itself"

# The sidebar Trash row.
m=$(mark); ptr move $(at "item omega.txt") click 272
wait_log "$m" "cursor omega.txt selected 1" || fail "could not select omega"
drag "$(at "item omega.txt")" "$(at "place Trash")"
wait_file '[[ ! -e $SRC/omega.txt ]] && ls "$HOME_DIR/.local/share/Trash/files" | grep -q omega' \
  || fail "dropping on Trash did not trash omega"
wait_rows "$m" "item gamma.txt" "item zeta.txt" || fail "the listing did not drop omega"

# Spring-load: rest on Box 900 ms, it opens; drop in its empty space; the view returns to Work (D15).
m=$(mark); ptr move $(at "item zeta.txt") click 272
wait_log "$m" "cursor zeta.txt selected 1" || fail "could not select zeta"
read -r bx by <<< "$(at "item Box")"
ptrk move $(at "item zeta.txt") pause 450 press 272 move $((bx - 20)) $((by - 20)) move "$bx" "$by" pause 900 \
  move $((vx + 100)) "$vy" pause 200 release 272
wait_log "$m" "spring-load $SRC/Box" || fail "resting on Box did not spring-load it"
wait_log "$m" "spring-load returns to $SRC\$" || fail "the view did not return after the drop"
wait_file '[[ -f $SRC/Box/zeta.txt && ! -e $SRC/zeta.txt ]]' || fail "the spring-loaded drop did not land in Box"
shot 3-end

# Split view (FILES-UI-SPEC §13): F3 opens a second pane on the folder and makes it active; the right pane goes into
# Archive; a press in the left pane makes it active; gamma dragged onto the right pane moves; F3 again closes it.
m=$(mark); ptrk tap 61
wait_log "$m" "split view on" || fail "F3 did not open the split view"
wait_log "$m" "split active pane right" || fail "the new pane did not become active"
m=$(mark); ptrk tap 30 tap 19 pause 100 tap 28    # "ar", Enter: Archive in the right pane
wait_log "$m" "location $SRC/Archive\$" || fail "the right pane did not open Archive"
shot 4-split
wait_rows "$m" "item alpha.txt" "item beta.txt" || fail "the right pane did not list Archive"
# Both panes log geometry under the same names: the left pane's gamma is the one with the smallest x.
read -r gx gy <<< "$(grep -E "geometry item gamma.txt " "$OUT/files.log" | awk '{print $(NF-1), $NF}' | sort -n | head -n 1)"
left_gamma="$((WX + gx)) $((WY + gy))"
m=$(mark); ptr move $left_gamma click 272
wait_log "$m" "split active pane left" || fail "a press in the left pane did not make it active"
# The pane switch rebuilds and logs alpha at rest; an earlier line may be from the right pane's slide.
wait_log "$m" "geometry item alpha.txt " || fail "the right pane did not log its rows after the switch"
m=$(mark); drag "$left_gamma" "$(at "item alpha.txt")"
wait_log "$m" "drop target $SRC/Archive" || fail "the right pane was not a drop target"
wait_file '[[ -f $SRC/Archive/gamma.txt && ! -e $SRC/gamma.txt ]]' || fail "the drop on the right pane did not move gamma"
shot 4b-split-drop
m=$(mark); ptrk tap 61
wait_log "$m" "split view off" || fail "F3 did not close the split view"
shot 5-split-closed
echo "pass files_select_dnd"
