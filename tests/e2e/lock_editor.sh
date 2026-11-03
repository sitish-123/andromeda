#!/usr/bin/env bash
# The lock screen editor (L6a) runs unlocked and edits a draft: `msg lockscreen-widgets-edit` opens it, keys pick the
# Glance layout in the Layout panel, Escape drops that draft (config unchanged), and Done saves it (layout = "glance").
# Writes editor.png, panel.png and noctalia.log to $OUT (default ./artifacts/lock-editor).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/lock-editor}
source "$(dirname "$0")/lib.sh"
rm -rf "$OUT"
boot_headless 1

with_noctalia '
  fail() { echo "FAIL: $*" >&2; exit 1; }
  msg() { "$NOCTALIA" msg "$@" 2>&1 || true; }
  wait_for() { local what=$1; shift; for _ in $(seq 50); do "$@" && return; sleep 0.1; done; fail "$what"; }
  opened() { grep -c "lock-editor.* opened on" "$OUT/noctalia.log" | grep -qx "$1"; }
  closed() { grep -c "lock-editor.* closed" "$OUT/noctalia.log" | grep -qx "$1"; }
  saved() { grep -rqs "layout = \"glance\"" "$HOME/.config/noctalia" "$HOME/.local/state/noctalia"; }
  keys() { for k in "$@"; do wtype -k "$k"; done; }
  # Focus order with the panel open: Slots, Layout, Theme, Done, Centered, Glance, Split, Minimal, Andromeda, Ryoku.
  pick_glance() { keys Tab Tab Return Tab Tab Tab Tab Return; }

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
  keys Escape Tab Tab Tab Tab Return
  wait_for "Done did not close the editor" closed 2
  wait_for "Done did not save layout = glance" saved
'
echo "PASS: lock editor opens, Escape discards, Done saves; artifacts: $OUT"
