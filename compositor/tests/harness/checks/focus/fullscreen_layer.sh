#!/usr/bin/env bash
# harness: outputs=2
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly WINDOW_LOG="$ANDROMEDA_RUNTIME_DIR/fullscreen-layer-window.log"
readonly PANEL_LOG="$ANDROMEDA_RUNTIME_DIR/fullscreen-layer-panel.log"
readonly SHOT="$ANDROMEDA_RUNTIME_DIR/fullscreen-layer.png"

assert_pixel() {
  local y=$1 expected=$2 actual=
  "$ANDROMEDA" settle > /dev/null
  grim -o HEADLESS-1 "$SHOT"
  actual=$("$ANDROMEDA_PIXEL_PROBE" "$SHOT" pixel 100 "$y")
  if [[ $actual != "$expected" ]]; then
    echo "expected RGB $expected at 100,$y, got $actual"
    return 1
  fi
}

cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'

[animation]
enabled = false

[input.focus]
follows_mouse = false
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null
home_x=$("$ANDROMEDA" outputs | awk '$1 == "HEADLESS-1" {found = 1; next} found && /Position:/ {split($2, p, ","); print p[1]; exit}')
pointer_hold 2560 720 move "$((home_x + 640))" 360 mod none
"$ANDROMEDA_SEAT_LOG_CLIENT" fullscreen-layer-window > "$WINDOW_LOG" 2>&1 &
await_events "$WINDOW_LOG" keyboard-enter 1
window_id=$("$ANDROMEDA" windows --json | jq -r '.[] | select(.title == "fullscreen-layer-window") | .id')
"$ANDROMEDA" msg window-toggle-fullscreen > /dev/null
assert_pixel 20 "51 136 204"

# Hidden surfaces may not receive frame callbacks, so wait for compositor mapping instead of client readiness.
wait_layers() {
  local expected=$1
  for _ in $(seq 60); do
    [[ $("$ANDROMEDA" layers --json | jq '[.[] | select(.mapped)] | length') == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected $expected mapped layers: $("$ANDROMEDA" layers --json)"
  return 1
}

assert_no_enter() {
  if [[ $(events "$PANEL_LOG" keyboard-enter) != 0 ]]; then
    echo "hidden top-layer panel received keyboard focus: $(cat "$PANEL_LOG")"
    return 1
  fi
}

key_count=0
for mode in on-demand exclusive; do
  "$ANDROMEDA_LAYER_CLIENT" HEADLESS-1 40 "keyboard=$mode" > "$PANEL_LOG" 2>&1 &
  panel_pid=$!
  wait_layers 1
  assert_pixel 20 "51 136 204"
  "$ANDROMEDA_POINTER_CLIENT" 2560 720 tap 57
  key_count=$((key_count + 1))
  await_events "$WINDOW_LOG" 'keyboard-key code=57 state=pressed' "$key_count"
  assert_no_enter

  "$ANDROMEDA" msg "window-focus:$window_id" > /dev/null
  "$ANDROMEDA_POINTER_CLIENT" 2560 720 tap 57
  key_count=$((key_count + 1))
  await_events "$WINDOW_LOG" 'keyboard-key code=57 state=pressed' "$key_count"
  assert_no_enter

  overlay_log="$ANDROMEDA_RUNTIME_DIR/fullscreen-overlay-$mode.log"
  "$ANDROMEDA_LAYER_CLIENT" HEADLESS-1 0 overlay-layer "keyboard=$mode" release-on-escape > "$overlay_log" 2>&1 &
  overlay_pid=$!
  await_events "$overlay_log" keyboard-enter 1
  assert_pixel 20 "255 0 0"
  "$ANDROMEDA_POINTER_CLIENT" 2560 720 tap 57 tap 1
  await_events "$overlay_log" 'keyboard-key code=57 state=1' 1
  await_events "$overlay_log" keyboard-leave 1
  "$ANDROMEDA_POINTER_CLIENT" 2560 720 tap 57
  key_count=$((key_count + 1))
  await_events "$WINDOW_LOG" 'keyboard-key code=57 state=pressed' "$key_count"
  assert_no_enter
  kill "$overlay_pid"
  wait "$overlay_pid" || true
  wait_layers 1
  assert_pixel 20 "51 136 204"
  "$ANDROMEDA" msg window-toggle-fullscreen > /dev/null
  "$ANDROMEDA" settle > /dev/null
  # Leaving fullscreen uncovers the panel: an exclusive one takes the seat back, an on-demand one waits for a click.
  if [[ $mode == on-demand ]]; then
    assert_no_enter
    "$ANDROMEDA_POINTER_CLIENT" 2560 720 move "$((home_x + 100))" 20 click 272
  fi
  await_events "$PANEL_LOG" keyboard-enter 1
  "$ANDROMEDA" msg window-toggle-fullscreen > /dev/null
  await_events "$PANEL_LOG" keyboard-leave 1
  assert_pixel 20 "51 136 204"
  "$ANDROMEDA_POINTER_CLIENT" 2560 720 tap 57
  key_count=$((key_count + 1))
  await_events "$WINDOW_LOG" 'keyboard-key code=57 state=pressed' "$key_count"
  kill "$panel_pid"
  wait "$panel_pid" || true
  wait_layers 0

  other_log="$ANDROMEDA_RUNTIME_DIR/other-output-$mode.log"
  "$ANDROMEDA_LAYER_CLIENT" HEADLESS-2 40 "keyboard=$mode" > "$other_log" 2>&1 &
  other_pid=$!
  await_events "$other_log" keyboard-enter 1
  "$ANDROMEDA_POINTER_CLIENT" 2560 720 tap 57
  await_events "$other_log" 'keyboard-key code=57 state=1' 1
  kill "$other_pid"
  wait "$other_pid" || true
  wait_layers 0
  "$ANDROMEDA_POINTER_CLIENT" 2560 720 tap 57
  key_count=$((key_count + 1))
  await_events "$WINDOW_LOG" 'keyboard-key code=57 state=pressed' "$key_count"
done

echo "fullscreen retains keyboard focus over top layers; overlays still take and release it"
