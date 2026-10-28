#!/usr/bin/env bash
# Real captures distinguish raw primaries and custom luminances on committed
# surface state, including synchronized children and color-object destruction.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'
[animation]
enabled = false
[appearance.blur]
enabled = false
[overview]
background_blur = false
[output.HEADLESS-1]
hdr = "auto"
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null

readonly OUTPUT_IMAGE="$ANDROMEDA_RUNTIME_DIR/parametric-output.png"
readonly WINDOW_IMAGE="$ANDROMEDA_RUNTIME_DIR/parametric-window.png"
request() {
  local command=$1 previous
  previous=$(events "$CLIENT_LOG" "{\"event\":\"snapshot\",\"label\":\"$command\"")
  printf '%s\n' "$command" >&"$CLIENT_FD"
  await_lines "$CLIENT_LOG" "{\"event\":\"snapshot\",\"label\":\"$command\"" "$((previous + 1))"
  "$ANDROMEDA" settle > /dev/null
}
expect_pixel() {
  local image=$1 expected_red=$2 expected_green=$3 expected_blue=$4 description=$5
  local red green blue
  read -r red green blue < <("$ANDROMEDA_PIXEL_PROBE" "$image" pixel 640 360)
  if ((red < expected_red - 2 || red > expected_red + 2
       || green < expected_green - 2 || green > expected_green + 2
       || blue < expected_blue - 2 || blue > expected_blue + 2)); then
    echo "$mode: $description rendered $red $green $blue, expected $expected_red $expected_green $expected_blue"
    exit 1
  fi
}
expect_hdr() {
  local requested=$1 description=$2 state
  for _ in $(seq 60); do
    state=$("$ANDROMEDA" color --json)
    if jq -e --argjson requested "$requested" '
      .outputs[0].hdr_mode == "auto" and .outputs[0].hdr_requested == $requested
      and .outputs[0].hdr_active == false
      and .outputs[0].fallback_reason == (if $requested then "display does not advertise PQ" else "" end)
    ' <<< "$state" > /dev/null; then
      return 0
    fi
    sleep 0.05
  done
  echo "$mode: $description did not request automatic HDR=$requested: $state"
  exit 1
}
expect_pixels() {
  local red=$1 green=$2 blue=$3 description=$4 requested=$5
  expect_hdr "$requested" "$description"
  grim -o HEADLESS-1 "$OUTPUT_IMAGE"
  grim -T "$window_id" "$WINDOW_IMAGE"
  expect_pixel "$OUTPUT_IMAGE" "$red" "$green" "$blue" "$description on the output"
  expect_pixel "$WINDOW_IMAGE" "$red" "$green" "$blue" "$description in the window capture"
}

for mode in toplevel subsurface subsurface-desync; do
  CLIENT_LOG="$ANDROMEDA_RUNTIME_DIR/parametric-$mode.log"
  fifo="$ANDROMEDA_RUNTIME_DIR/parametric-$mode.fifo"
  mkfifo "$fifo"
  args=(custom)
  [[ $mode == toplevel ]] || args+=("$mode")
  "$ANDROMEDA_COLOR_FEEDBACK_CLIENT" "${args[@]}" < "$fifo" > "$CLIENT_LOG" 2>&1 &
  client_pid=$!
  exec {CLIENT_FD}> "$fifo"
  await_lines "$CLIENT_LOG" '{"event":"mapped"}' 1
  "$ANDROMEDA" settle > /dev/null
  window_id=$("$ANDROMEDA" windows --json | jq -r '.[] | select(.app_id == "color-feedback-client") | .id')

  # Input is RGB 128/112/96. Gamma22 decoding, the standard BT.2020 to
  # sRGB matrix and piecewise sRGB output encoding produce 139/110/93.
  expect_pixels 139 110 93 'named BT.2020 gamma22' false
  request 'custom 1'
  expect_pixels 139 110 93 'equivalent raw BT.2020 gamma22' false
  request 'custom 2'
  expect_pixels 129 113 96 'raw sRGB with maximum and reference 80' false
  request 'custom 3'
  expect_pixels 177 155 133 'raw sRGB with maximum 160 and reference 80' true
  request 'custom 4'
  expect_pixels 129 113 96 'reference-only change from 80 to 160' false
  request 'custom 3'
  expect_pixels 177 155 133 'reference restored to 80' true
  request unset
  expect_pixels 128 112 96 'unset custom luminances' false
  request 'custom 3'
  expect_pixels 177 155 133 'custom luminances restored after unset' true
  request destroy-only
  expect_pixels 128 112 96 'destroyed color object without a client surface commit' false
  request recreate
  expect_pixels 139 110 93 'recreated named BT.2020 description' false
  request 'custom 3'
  expect_pixels 177 155 133 'custom state after color-object recreation' true

  # Overview mirrors into separate scene buffers. Updating only the color
  # description must update the visible preview rather than retain old values.
  "$ANDROMEDA" msg overview-open > /dev/null
  "$ANDROMEDA" settle > /dev/null
  grim -o HEADLESS-1 "$OUTPUT_IMAGE"
  expect_pixel "$OUTPUT_IMAGE" 177 155 133 'custom luminances in the overview preview'
  request 'custom 4'
  grim -o HEADLESS-1 "$OUTPUT_IMAGE"
  expect_pixel "$OUTPUT_IMAGE" 129 113 96 'reference-only change in the overview preview'
  request 'custom 1'
  grim -o HEADLESS-1 "$OUTPUT_IMAGE"
  expect_pixel "$OUTPUT_IMAGE" 139 110 93 'raw BT.2020 primaries in the overview preview'
  request 'custom 3'
  "$ANDROMEDA" msg overview-close > /dev/null
  "$ANDROMEDA" settle > /dev/null
  expect_pixels 177 155 133 'custom state restored after overview' true

  if [[ $mode == subsurface ]]; then
    request cache
    expect_pixels 177 155 133 'custom state while synchronized unset is cached' true
    request parent
    expect_pixels 128 112 96 'cached unset applied by the parent' false
    request apply
    expect_pixels 177 155 133 'pending custom state applied by the child and parent' true
  fi

  kill -TERM "$client_pid"
  wait "$client_pid" || true
  exec {CLIENT_FD}>&-
  "$ANDROMEDA" settle > /dev/null
  expect_hdr false 'client teardown'
done

echo 'raw primaries, luminance and automatic HDR transitions verified in output, window captures and overview previews'
