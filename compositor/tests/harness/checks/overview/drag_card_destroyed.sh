#!/usr/bin/env bash
# A window that floats or closes while its overview card is pressed or dragged ends that press or drag: the next
# motion and release must not reach the freed card. Run with MALLOC_PERTURB_ set so a stale card read crashes.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly BTN_LEFT=272

cat >> "$ANDROMEDA_CONFIG" <<'EOF'

[animation]
enabled = false

[overview]
style = "strip"
zoom = 0.5
EOF
"$ANDROMEDA" msg config-reload > /dev/null

open_window() {
  local id
  "$ANDROMEDA_UNMAP_CLIENT" "$1" 400 300 > "$ANDROMEDA_RUNTIME_DIR/$1.log" 2>&1 &
  for _ in $(seq 80); do
    id=$("$ANDROMEDA" windows --json | jq -r --arg t "$1" '.[] | select(.title == $t) | .id')
    [[ -n $id ]] && { echo "$id"; return 0; }
    sleep 0.025
  done
  echo "$1 never mapped" >&2
  return 1
}

alive() {
  if ! "$ANDROMEDA" windows --json > /dev/null 2>&1; then
    echo "the compositor died after $1"
    exit 1
  fi
}

# Drags the tile's card, runs one IPC action mid-drag, then moves and releases. The 1280x720 output previews at half
# size from 320,180.
drag_through() {
  local label=$1 action=$2 x y
  read -r x y < <("$ANDROMEDA" windows --json | jq -r --arg id "${action##*:}" \
    '.[] | select(.id == $id) | "\(320 + (.x + .w / 2) / 2 | floor) \(180 + (.y + .h / 2) / 2 | floor)"')
  "$ANDROMEDA" msg overview-open > /dev/null
  "$ANDROMEDA" settle
  pointer_hold 1280 720 move "$x" "$y" press "$BTN_LEFT" move $((x + 30)) $((y + 20)) move $((x + 40)) $((y + 30)) -- \
    move $((x + 60)) $((y + 40)) move $((x + 80)) $((y + 50)) mark moved hold release "$BTN_LEFT"
  "$ANDROMEDA" msg "$action" > /dev/null
  echo "$label: pressed $x,$y" >&2
  "$ANDROMEDA" settle
  pointer_step moved
  alive "$label then motion"
  pointer_release
  "$ANDROMEDA" settle
  alive "$label then release"
  "$ANDROMEDA" msg overview-close > /dev/null 2>&1 || true
  "$ANDROMEDA" settle
}

floater=$(open_window floater)
drag_through "floating the dragged window" "window-toggle-floating:$floater"

closer=$(open_window closer)
drag_through "closing the dragged window" "window-close:$closer"

echo "floating or closing the dragged window ended the drag and left the compositor running"
