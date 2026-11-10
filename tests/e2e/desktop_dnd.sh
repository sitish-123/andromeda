#!/usr/bin/env bash
# Desktop drag and drop and Stacks (FB-DESKTOP, .agent/FILES-UI-SPEC.md §20) on the shell desktop and dock.
# Fails when:
#   - a file dragged from a GTK 4 app onto empty desktop is not copied into ~/Desktop,
#   - a file dragged from the GTK app onto the desktop folder "Inbox" is not copied into that folder,
#   - the desktop icon "Inbox" dragged onto the GTK app does not reach it as a file URI (text/uri-list),
#   - a file dragged from Files to the screen edge does not show the auto-hidden dock, a drop on the pinned app
#     "Drop Catch" does not open the file with it (%F), or the dock does not hide again after the drag,
#   - Ctrl+Z on the desktop does not undo the copy into Inbox, a desktop icon dropped on Inbox is not moved in, or
#     Ctrl+Z does not move it back (FileOps1 jobs),
#   - Use Stacks does not fold the two images into one "Images" stack that a click opens and a second click closes,
#     or Delete on one file picked inside the open stack trashes more than that file.
# Shots and logs go to $OUT (default ./artifacts/desktop-dnd).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/desktop-dnd}
source "$(dirname "$0")/lib.sh"
POINTER=$ROOT/compositor/build-debug/tests/pointer-client
FILES=${FILES:-$ROOT/apps/files/build/andromeda-files}
FILEOPS=${FILEOPS:-$ROOT/system/target/debug/andromeda-fileops}

boot_headless 1
printf '\n[layout]\nmode = "floating"\n' >> "$RUNTIME/andromeda.toml"
run "$ANDROMEDA" msg config-reload > /dev/null
H=$RUNTIME/home
DESK=$H/Desktop
mkdir -p "$DESK/Inbox" "$H/Docs" "$H/.local/share/applications"
cp "$ROOT/shell/assets/andromeda-wallpaper.png" "$DESK/beach.png"
cp "$ROOT/shell/assets/andromeda-wallpaper.png" "$DESK/forest.png"
echo outside > "$H/outside.txt"
echo report > "$H/Docs/report.txt"
cat > "$H/.local/share/applications/dropcatch.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=Drop Catch
Exec=sh -c 'for f; do echo "\$f"; done >> $OUT/caught.txt' sh %F
Icon=text-x-generic
MimeType=text/plain;
DESKTOP
rm -f "$OUT/caught.txt"
cat >> "$H/.config/andromeda/shell/config.toml" <<'CONFIG'

[desktop_icons]
enabled = true
start_corner = "top_right"

[dock]
enabled = true
auto_hide = true
smart_auto_hide = false
pinned = ["dropcatch"]
CONFIG

export POINTER FILES FILEOPS DESK H
export ANDROMEDA_LOG_LEVEL=debug DESK_DOCK_GEOMETRY=1
with_noctalia '
  fail() { echo "FAIL: $*"; exit 1; }
  wait_for() { # description, test command...
    local what=$1; shift
    for _ in $(seq 100); do "$@" && return 0; sleep 0.05; done
    fail "$what"
  }
  logged() { grep -qsE -- "$2" "$1"; }
  settle() { "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null; }
  # The headless seat has a keyboard only while a virtual keyboard exists, and the shell binds wl_keyboard when it
  # appears: a key sent in the same burst as its click was lost while the shell bound it (log: "keyboard: bound" and
  # "released" around the press, no key). One keyboard held for the whole test keeps it bound.
  tail -f /dev/null | "$POINTER" 1280 720 keyboard-only mod none hold > /dev/null &
  keyboard_bound() { [[ $(grep -c "keyboard: bound" "$OUT/noctalia.log") -gt $(grep -c "keyboard: released" "$OUT/noctalia.log") ]]; }
  for _ in $(seq 100); do keyboard_bound && break; sleep 0.05; done
  keyboard_bound || { echo "FAIL: the held keyboard did not bind"; exit 1; }
  # Desktop copies and moves are FileOps1 jobs: the daemon runs on this test bus.
  "$FILEOPS" > "$OUT/fileops.log" 2>&1 &
  wait_for "the FileOps1 daemon is not on the bus" gdbus introspect --session -d org.andromeda.FileOps1 -o /org/andromeda/FileOps1
  # A press, steps past the drag threshold, a rest on the target and the release.
  drag() { # x0 y0 x1 y1
    "$POINTER" 1280 720 move "$1" "$2" pause 300 press 272 move $(($1 + 6)) $(($2 + 6)) move $(($1 + 20)) $(($2 + 20)) \
      move $((($1 + $3) / 2)) $((($2 + $4) / 2)) move "$3" "$4" pause 200 move $(($3 + 1)) "$4" pause 100 release 272 > /dev/null
  }
  layer() { "$ANDROMEDA" layers --json | jq -c "[.[] | select(.namespace == \"$1\")][0]"; }
  win() { "$ANDROMEDA" windows --json | jq -c "[.. | objects | select(.title? == \"$1\")][0] // empty"; }

  wait_for "the desktop did not map" bash -c "[[ \$(\"$ANDROMEDA\" layers --json | jq \"[.[] | select(.namespace == \\\"andromeda-desktop-icons\\\" and .w > 0)] | length\") -gt 0 ]]"
  settle
  desk=$(layer andromeda-desktop-icons)
  DX=$(jq .x <<< "$desk"); DY=$(jq .y <<< "$desk"); DW=$(jq .w <<< "$desk")
  # Folders first: Inbox is the top-right cell (96 x 104, 16 from the edge, plate 72).
  INBOX_X=$((DX + DW - 16 - 48)); INBOX_Y=$((DY + 16 + 36))

  GDK_BACKEND=wayland python3 -I "'"$ROOT"'/tests/e2e/dnd_app.py" dnd-app "$H/outside.txt" > "$OUT/dnd-app.log" 2>&1 &
  wait_for "the GTK app did not map" bash -c "[[ -n \$(\"$ANDROMEDA\" windows --json | jq -r \".. | objects | select(.title? == \\\"dnd-app\\\") | .title\") ]]"
  settle
  w=$(win dnd-app)
  WX=$(jq .x <<< "$w"); WY=$(jq .y <<< "$w"); WW=$(jq .w <<< "$w"); WH=$(jq .h <<< "$w")
  SRC_X=$((WX + WW / 4)); SRC_Y=$((WY + WH / 2)); DROP_X=$((WX + 3 * WW / 4))

  # 1. GTK app -> empty desktop (left of the window).
  drag "$SRC_X" "$SRC_Y" $((DX + 120)) $((DY + 120))
  wait_for "the GTK drag did not copy outside.txt to the desktop" test -f "$DESK/outside.txt"
  echo "PASS GTK app to the desktop"

  # 2. GTK app -> the Inbox folder icon.
  drag "$SRC_X" "$SRC_Y" "$INBOX_X" "$INBOX_Y"
  wait_for "the GTK drag did not copy outside.txt into Inbox" test -f "$DESK/Inbox/outside.txt"
  echo "PASS GTK app to a desktop folder"

  # Ctrl+Z on the desktop undoes that copy.
  "$POINTER" 1280 720 move $((DX + 120)) $((DY + 400)) click 272 mod control tap 44 mod none > /dev/null
  wait_for "Ctrl+Z did not undo the copy into Inbox" bash -c "[[ ! -e \"$DESK/Inbox/outside.txt\" ]]"
  echo "PASS undo of a copy"

  # A desktop icon dropped on Inbox moves in (FileOps1 move job); Ctrl+Z puts it back.
  COL=$(( (DW - 16 - 120) / 96 ))
  drag $((DX + DW - 16 - COL * 96 - 48)) $((DY + 16 + 104 + 36)) "$INBOX_X" "$INBOX_Y"
  wait_for "the desktop drag did not move outside.txt into Inbox" bash -c "[[ -f \"$DESK/Inbox/outside.txt\" && ! -e \"$DESK/outside.txt\" ]]"
  logged "$OUT/noctalia.log" "desktop drop moves 1 items into Inbox" || fail "the drop into Inbox was not a move"
  "$POINTER" 1280 720 move $((DX + 120)) $((DY + 400)) click 272 mod control tap 44 mod none > /dev/null
  wait_for "Ctrl+Z did not bring outside.txt back" bash -c "[[ -f \"$DESK/outside.txt\" && ! -e \"$DESK/Inbox/outside.txt\" ]]"
  echo "PASS move into a desktop folder and undo"

  # 3. Desktop icon -> GTK app: the app gets the file URI.
  settle
  drag "$INBOX_X" "$INBOX_Y" "$DROP_X" "$SRC_Y"
  wait_for "the GTK app got no URI from the desktop drag" logged "$OUT/dnd-app.log" "^dropped file://.*/Desktop/Inbox$"
  [[ -d $DESK/Inbox ]] || fail "the drag moved Inbox off the desktop"
  echo "PASS desktop icon to the GTK app"

  # 4. Files -> the pinned dock app: it opens the file (Exec %F).
  env DESK_FILES_GEOMETRY=1 ANDROMEDA_LOG_LEVEL=debug "$FILES" "$H/Docs" > "$OUT/files.log" 2>&1 &
  FILES_PID=$!
  wait_for "Files did not list report.txt" logged "$OUT/files.log" "geometry item report.txt "
  settle
  f=$(win Docs)
  [[ -n $f ]] || f=$("$ANDROMEDA" windows --json | jq -c "[.. | objects | select(.app_id? == \"dev.andromeda.Files\")][0]")
  read -r rx ry <<< "$(grep -E "geometry item report.txt " "$OUT/files.log" | tail -n 1 | awk "{print \$(NF-1), \$NF}")"
  dock=$(layer noctalia-dock)
  read -r ix iy iw ih <<< "$(grep -E "dock item dropcatch " "$OUT/noctalia.log" | tail -n 1 | awk "{print \$(NF-3), \$(NF-2), \$(NF-1), \$NF}")"
  [[ -n ${ix:-} ]] || fail "the dock logged no item rect for dropcatch"
  # The dock auto-hides: the drag first reaches the bottom edge, where the hidden dock keeps its strip, then the app.
  fx=$(( $(jq .x <<< "$f") + rx )); fy=$(( $(jq .y <<< "$f") + ry ))
  tx=$(( $(jq .x <<< "$dock") + ix + iw / 2 )); ty=$(( $(jq .y <<< "$dock") + iy + ih / 2 ))
  "$POINTER" 1280 720 move "$fx" "$fy" pause 300 press 272 move $((fx + 6)) $((fy + 6)) move $((fx + 20)) $((fy + 20)) \
    move "$tx" 719 mark edge pause 400 move "$tx" "$ty" pause 200 move $((tx + 1)) "$ty" pause 100 release 272 > /dev/null
  logged "$OUT/noctalia.log" "dock shown for a drag" || fail "the file drag did not show the hidden dock"
  wait_for "the dock app did not open report.txt" logged "$OUT/caught.txt" "^$H/Docs/report.txt$"
  grep -q "dock drop on dropcatch" "$OUT/noctalia.log" || fail "the dock did not log the drop"
  wait_for "the dock did not hide after the drag" logged "$OUT/noctalia.log" "dock hides after a drag"
  echo "PASS Files to a dock app"
  grim "$OUT/after-dock.png"
  # Files covers the right column of icons: it closes before the stacks step.
  kill "$FILES_PID"
  wait_for "the Files window did not close" bash -c "[[ -z \$(\"$ANDROMEDA\" windows --json | jq -r \".. | objects | select(.app_id? == \\\"dev.andromeda.Files\\\") | .app_id\") ]]"

  # 5. Use Stacks: the two images fold into one Images stack; a click opens it, a second click closes it.
  # The desktop menu toggle writes this key; the config watcher applies it.
  sed -i "s/^start_corner = \"top_right\"$/start_corner = \"top_right\"\nstacks = true/" "$H/.config/andromeda/shell/config.toml"
  wait_for "the desktop did not switch to stacks" logged "$OUT/noctalia.log" "desktop stacks on"
  settle
  grim "$OUT/stacks.png"
  # Order with stacks: Inbox, then the Images stack, then the documents (outside.txt alone).
  STACK_X=$INBOX_X; STACK_Y=$((INBOX_Y + 104))
  "$POINTER" 1280 720 move "$STACK_X" "$STACK_Y" click 272 > /dev/null
  wait_for "a click did not open the Images stack" logged "$OUT/noctalia.log" "stack Images expanded"
  settle
  grim "$OUT/stack-open.png"
  "$POINTER" 1280 720 move "$STACK_X" "$STACK_Y" click 272 > /dev/null
  wait_for "a second click did not close the Images stack" logged "$OUT/noctalia.log" "stack Images collapsed"
  # One file picked inside an open stack is only that file: Delete trashes beach.png, forest.png stays.
  "$POINTER" 1280 720 move "$STACK_X" "$STACK_Y" click 272 > /dev/null
  wait_for "the Images stack did not open again" bash -c "[[ \$(grep -c \"stack Images expanded\" \"$OUT/noctalia.log\") -ge 2 ]]"
  settle
  # Key and click go in two steps: in one burst the shell can read Delete before the click selected the file.
  for _ in $(seq 40); do
    "$POINTER" 1280 720 move "$STACK_X" $((STACK_Y + 104)) click 272 > /dev/null
    logged "$OUT/noctalia.log" "desktop press beach.png" && break
    sleep 0.05
  done
  logged "$OUT/noctalia.log" "desktop press beach.png" || fail "the click did not reach beach.png in the open stack"
  "$POINTER" 1280 720 keyboard-only tap 111 > /dev/null
  wait_for "Delete did not trash beach.png" bash -c "[[ ! -e \"$DESK/beach.png\" ]]"
  settle
  [[ -f $DESK/forest.png ]] || fail "Delete on one file of an open stack trashed the whole stack"
  echo "PASS Use Stacks"
'
ls "$OUT"/*.png
