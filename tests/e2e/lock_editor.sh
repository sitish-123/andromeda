#!/usr/bin/env bash
# The lock screen editor (L6a) runs unlocked and edits a draft: `msg lockscreen-widgets-edit` opens it, keys pick the
# Glance layout in the Layout panel, Escape drops that draft (config unchanged), Done saves it (layout = "glance"), and
# the Slots clock gallery saves a picked style (clock_style = "bold"), and the widget row and bottom slot save
# widgets = ["weather"] and bottom_slot = "phone", Free saves a placed, arrow-moved widget (layout = "custom"), and a
# layout pick after that asks first, and the Wallpaper panel saves a picked colour (L7). Writes editor.png, panel.png,
# gallery.png, slots.png, free.png, alert.png, wallpaper.png and noctalia.log to $OUT (default ./artifacts/lock-editor).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/lock-editor}
source "$(dirname "$0")/lib.sh"
rm -rf "$OUT"
boot_headless 1

with_noctalia '
  fail() { echo "FAIL: $*" >&2; exit 1; }
  msg() { "$NOCTALIA" msg "$@" 2>&1 || true; }
  wait_for() { local what=$1; shift; for _ in $(seq 50); do "$@" && return; sleep 0.1; done; fail "$what"; }
  # Keys typed before the editor has the keyboard go to the old focus: wait for its "keyboard focus" line.
  opened() { grep -c "lock-editor.* keyboard focus" "$OUT/noctalia.log" | grep -qx "$1"; }
  closed() { grep -c "lock-editor.* closed" "$OUT/noctalia.log" | grep -qx "$1"; }
  saved() { grep -rqs "layout = \"glance\"" "$HOME/.config/noctalia" "$HOME/.local/state/noctalia"; }
  keys() { for k in "$@"; do wtype -k "$k"; done; }
  # Focus order with the panel open: Slots, Free, Wallpaper, Layout, Theme, Done, Centered, Glance, Split, Minimal,
  # Andromeda, Ryoku.
  pick_glance() { keys Tab Tab Tab Tab Return Tab Tab Tab Tab Return; }

  wtype -s 3600000 &
  sleep 0.3
  [[ $(msg lockscreen-widgets-edit) == ok ]] || fail "editor refused to open"
  wait_for "editor did not open" opened 1
  uploads() { [[ $(grep -c "texcache.* uploaded" "$OUT/noctalia.log") -ge 2 ]]; }
  wait_for "the blurred preview image did not load" uploads
  sleep 0.3 # real time: the frame after the upload
  grim "$OUT/editor.png"
  pick_glance
  sleep 0.5 # real time: one frame
  grim "$OUT/panel.png"
  keys Escape Escape
  wait_for "Escape did not close the editor" closed 1
  saved && fail "Escape saved the draft"

  [[ $(msg lockscreen-widgets-edit) == ok ]] || fail "editor refused to open again"
  wait_for "editor did not open again" opened 2
  pick_glance
  keys Escape Tab Tab Tab Tab Tab Tab Return
  wait_for "Done did not close the editor" closed 2
  wait_for "Done did not save layout = glance" saved

  # Slots: Tab past the bar to the clock slot, Return opens the gallery, Tab Tab = the second style (bold).
  bold() { grep -rqs "clock_style = \"bold\"" "$HOME/.config/noctalia" "$HOME/.local/state/noctalia"; }
  [[ $(msg lockscreen-widgets-edit) == ok ]] || fail "editor refused to open a third time"
  wait_for "editor did not open a third time" opened 3
  keys Tab Tab Tab Tab Tab Tab Tab Return Tab Tab Return
  sleep 0.5 # real time: one frame
  grim "$OUT/gallery.png"
  keys Escape Tab Tab Tab Tab Tab Tab Return
  wait_for "Done did not close the editor after the gallery" closed 3
  wait_for "Done did not save clock_style = bold" bold

  # Step 2 saved Glance (no row): the Layout panel picks Centered (3 Tabs past Layout), Escape closes the panel.
  # Slots: the first row place opens the widget gallery on Weather (Return picks it); four Tabs on = the bottom slot,
  # whose chooser opens on Media; Tab Return picks Phone; four Tabs on = Done.
  # The writer may wrap the array over lines: match the key, then "weather" in the next lines.
  widgets() { grep -rhsA3 "^ *widgets = \[" "$HOME/.config/noctalia" "$HOME/.local/state/noctalia" | grep -q "\"weather\""; }
  dump() { grep -rhsA12 "^\[lockscreen\]" "$HOME/.config/noctalia" "$HOME/.local/state/noctalia" >&2 || true; }
  phone() { grep -rqs "bottom_slot = \"phone\"" "$HOME/.config/noctalia" "$HOME/.local/state/noctalia"; }
  [[ $(msg lockscreen-widgets-edit) == ok ]] || fail "editor refused to open a fourth time"
  wait_for "editor did not open a fourth time" opened 4
  keys Tab Tab Tab Tab Return Tab Tab Tab Return
  sleep 0.5 # real time: one frame
  grim "$OUT/centered.png"
  keys Escape
  keys Tab Tab Tab Tab Tab Tab Tab Tab Return Return Tab Tab Tab Tab Return
  sleep 0.5 # real time: one frame
  grim "$OUT/slots.png"
  keys Tab Return Tab Tab Tab Tab Tab Tab Return
  wait_for "Done did not close the editor after the slots" closed 4
  for _ in $(seq 50); do widgets && break; sleep 0.1; done
  widgets || { dump; fail "Done did not save widgets = [\"weather\"]"; }
  wait_for "Done did not save bottom_slot = phone" phone

  # Free: Tab Tab Return picks the Free segment; Tab Return turns Snap to zones off (drops on the 32 grid); five Tabs
  # on (Wallpaper, Layout, Theme, Done) = the first add chip (Clock), Return adds it selected. Right Right moves it 8 (no snap back to 32); two Shift+Tabs (the new item, then Done)
  # reach Done. Saved: a clock widget whose centre x is 24 past the 32 grid (snapped x + 80 + 8), layout = "custom".
  # Exit 0 when a lock clock widget exists; with "moved", only one whose centre x is 24 past the 32 grid.
  lock_clock() {
    python3 - "$HOME" "$1" <<"PY"
import pathlib, sys, tomllib
home, mode = pathlib.Path(sys.argv[1]), sys.argv[2]
for f in list((home / ".config/noctalia").rglob("*.toml")) + list((home / ".local/state/noctalia").rglob("*.toml")):
    data = tomllib.loads(f.read_text())
    for w in data.get("lockscreen_widgets", {}).get("widget", {}).values():
        if w.get("type") == "clock" and (mode != "moved" or round(w.get("cx", -1)) % 32 == 24):
            sys.exit(0)
sys.exit(1)
PY
  }
  free_saved() { lock_clock moved; }
  custom() { grep -rqs "layout = \"custom\"" "$HOME/.config/noctalia" "$HOME/.local/state/noctalia"; }
  [[ $(msg lockscreen-widgets-edit) == ok ]] || fail "editor refused to open a fifth time"
  wait_for "editor did not open a fifth time" opened 5
  keys Tab Tab Return Tab Return Tab Tab Tab Tab Tab Return Right Right
  sleep 0.5 # real time: one frame
  grim "$OUT/free.png"
  keys ISO_Left_Tab ISO_Left_Tab Return
  wait_for "Done did not close the editor after Free" closed 5
  wait_for "Done did not save the moved clock widget" free_saved
  wait_for "Done did not save layout = custom" custom

  # Layout change with placed widgets asks first (rulebook 9.10 alert, Cancel focused): Return cancels, the pick again,
  # Tab Return confirms; Escape closes the panel, five Tabs = Done. Saved: layout = "glance", the clock widget gone.
  glance() { grep -rqs "layout = \"glance\"" "$HOME/.config/noctalia" "$HOME/.local/state/noctalia"; }
  no_clock() { ! lock_clock any; }
  [[ $(msg lockscreen-widgets-edit) == ok ]] || fail "editor refused to open a sixth time"
  wait_for "editor did not open a sixth time" opened 6
  pick_glance
  sleep 0.5 # real time: one frame
  grim "$OUT/alert.png"
  keys Return Return Tab Return Escape Tab Tab Tab Tab Tab Tab Return
  wait_for "Done did not close the editor after the layout alert" closed 6
  wait_for "Done did not save layout = glance after the alert" glance
  wait_for "the confirmed layout change kept the clock widget" no_clock

  # Wallpaper (L7): three Tabs + Return open the Wallpaper panel; six Tabs on (Layout, Theme, Done, Photos, Live,
  # Colours) + Return show the colours; Tab Tab (the scope chip, the first colour) + Return pick night blue; five
  # Shift+Tabs (scope, Colours, Live, Photos, Done) + Return save lockscreen.wallpaper = "color:#10182b".
  colour() { grep -rqs "wallpaper = \"color:#10182b\"" "$HOME/.config/noctalia" "$HOME/.local/state/noctalia"; }
  [[ $(msg lockscreen-widgets-edit) == ok ]] || fail "editor refused to open a seventh time"
  wait_for "editor did not open a seventh time" opened 7
  keys Tab Tab Tab Return Tab Tab Tab Tab Tab Tab Return Tab Tab Return
  sleep 0.5 # real time: one frame
  grim "$OUT/wallpaper.png"
  keys ISO_Left_Tab ISO_Left_Tab ISO_Left_Tab ISO_Left_Tab ISO_Left_Tab Return
  wait_for "Done did not close the editor after the wallpaper pick" closed 7
  wait_for "Done did not save the colour wallpaper" colour
'
echo "PASS: lock editor opens, Escape discards, Done saves; artifacts: $OUT"
