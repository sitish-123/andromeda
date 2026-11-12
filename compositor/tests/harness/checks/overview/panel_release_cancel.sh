#!/usr/bin/env bash
# A left press on the overview's empty desktop, released over a panel, is a cancel: the overview stays open. The same
# press released where it began closes the overview to that desktop.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly BTN_LEFT=272 W=1280 H=720
EVENTS="$ANDROMEDA_RUNTIME_DIR/overview-events.log"

cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'
[animation]
enabled = false
[overview]
style = "spaces"
background_blur = false
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null

PANEL_LOG="$ANDROMEDA_RUNTIME_DIR/panel.log"
"$ANDROMEDA_LAYER_CLIENT" HEADLESS-1 40 > "$PANEL_LOG" 2>&1 &
await_events "$PANEL_LOG" ready 1 "panel"
"$ANDROMEDA" subscribe overview > "$EVENTS" &
# Closes after the first open: the subscription starts with the current, closed state.
closes() {
  jq -s '[.[] | select(.event == "overview") | .data.open] | .[(index(true) // length):] | map(select(. == false)) | length' \
    "$EVENTS"
}

"$ANDROMEDA" msg overview-open > /dev/null
"$ANDROMEDA" settle > /dev/null
# The main area of the empty desktop, then the panel along the top edge.
"$ANDROMEDA_POINTER_CLIENT" "$W" "$H" move 640 450 press "$BTN_LEFT" move 640 300 move 1160 20 release "$BTN_LEFT"
"$ANDROMEDA" settle > /dev/null
(($(closes) == 0)) || { echo "a desktop press released over the panel closed the overview: $(< "$EVENTS")"; exit 1; }

"$ANDROMEDA_POINTER_CLIENT" "$W" "$H" move 640 450 click "$BTN_LEFT"
"$ANDROMEDA" settle > /dev/null
(($(closes) == 1)) || { echo "a click on the empty desktop did not close the overview: $(< "$EVENTS")"; exit 1; }
echo "a desktop press released over a panel leaves the overview open; a click there closes it"
