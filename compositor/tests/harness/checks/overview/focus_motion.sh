#!/usr/bin/env bash
# Selecting a partially hidden scrolling column from the overview must update focus while the closing zoom is active.
# That early focus starts the strip reveal on the same frame and timeline as the zoom. If focus is deferred until
# teardown, this assertion observes the old focused column for the whole close animation and the movement begins only
# after the zoom has landed.
set -euo pipefail

readonly BTN_LEFT=272
readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly POINTER="${ANDROMEDA_POINTER_CLIENT:-./build-debug/tests/pointer-client}"

# The pointer client needs the output size to normalise absolute coordinates.
pointer() {
  "$POINTER" "$OUTPUT_W" "$OUTPUT_H" "$@"
}

spawn_client() {
  foot --title="$1" sh -c 'sleep 120' > /dev/null 2>&1 &
}

wait_for_count() {
  for _ in $(seq 60); do
    [[ $("$ANDROMEDA" windows --json | jq 'length') -eq $1 ]] && return 0
    sleep 0.25
  done
  echo "timed out waiting for $1 window(s)"
  return 1
}

printf '\n[animation.overview]\nduration_ms = 2000\ncurve = "linear"\n' >> "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null

spawn_client overview-motion-first
wait_for_count 1
spawn_client overview-motion-second
wait_for_count 2
spawn_client overview-motion-third
wait_for_count 3

first_id=$("$ANDROMEDA" windows --json | jq -r '.[] | select(.title == "overview-motion-first") | .id')
if [[ -z $first_id ]]; then
  echo "could not resolve the first overview motion window: $("$ANDROMEDA" windows --json)"
  exit 1
fi
"$ANDROMEDA" msg "window-focus:$first_id" > /dev/null
"$ANDROMEDA" settle

"$ANDROMEDA" msg overview-open > /dev/null
"$ANDROMEDA" settle

# At zoom 0.5 the third 624-pixel column is visible from x=961 to x=1273. Click its visible center, then sample focus
# well before the two-second close animation can finish.
pointer move 1110 360 click "$BTN_LEFT"
sleep 0.1

focused_title=$("$ANDROMEDA" windows --json | jq -r '[.[] | select(.focused) | .title] | if length == 1 then .[0] else "none" end')
if [[ $focused_title != overview-motion-third ]]; then
  echo "overview selection deferred focus until after zoom: focused '$focused_title'"
  echo "  windows: $("$ANDROMEDA" windows --json | jq -c '[.[] | {title, x, focused}]')"
  exit 1
fi

echo "overview selection starts column focus and zoom together"
