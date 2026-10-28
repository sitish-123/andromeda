#!/usr/bin/env bash
# harness: xwayland=true
# Xwayland decoration hints hide or restore the negotiated title bar; rules can override them.
set -euo pipefail
source "$(dirname "$ANDROMEDA_HARNESS_LIB")/title_bar.sh"
title_bar_config
fifo=$ANDROMEDA_RUNTIME_DIR/x11-control
mkfifo "$fifo"
exec {control}<>"$fifo"
"$ANDROMEDA_XWAYLAND_FOCUS_CLIENT" ssd-x11 <&"$control" > "$ANDROMEDA_RUNTIME_DIR/x11.log" 2>&1 &
title_bar_wait_field ssd-x11 title_bar true
"$ANDROMEDA" settle > /dev/null
window=$(title_bar_window ssd-x11)
x=$(($(jq -r .x <<< "$window") + 5))
y=$(($(jq -r .y <<< "$window") - 14))
check_bar_pixel() {
  local expected=$1 red green blue
  "$ANDROMEDA" settle > /dev/null
  grim -o HEADLESS-1 "$ANDROMEDA_RUNTIME_DIR/x11.png"
  read -r red green blue < <("$ANDROMEDA_PIXEL_PROBE" "$ANDROMEDA_RUNTIME_DIR/x11.png" pixel "$x" "$y")
  if [[ $expected == on ]] && ((red > 240 && green < 12 && blue > 240)); then
    return 0
  fi
  if [[ $expected == off ]] && ((red < 12 && green < 12 && blue < 12)); then
    return 0
  fi
  echo "expected Xwayland title bar $expected, got pixel $red $green $blue"
  return 1
}
check_bar_pixel on
printf 'decorations off\n' >&"$control"
title_bar_wait_field ssd-x11 title_bar false
check_bar_pixel off
printf 'decorations on\n' >&"$control"
title_bar_wait_field ssd-x11 title_bar true
check_bar_pixel on
cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'
[[window_rule]]
match.title = "^ssd-x11$"
title_bar = "off"
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null
title_bar_wait_field ssd-x11 title_bar false
printf 'decorations off\n' >&"$control"
sed -i 's/title_bar = "off"/title_bar = "forced"/' "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null
title_bar_wait_field ssd-x11 title_bar true
sed -i 's/title_bar = "forced"/title_bar = "on"/' "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null
title_bar_wait_field ssd-x11 title_bar false
printf 'decorations on\n' >&"$control"
title_bar_wait_field ssd-x11 title_bar true
# Negotiated mode continues to follow Xwayland hints without Wayland negotiation.
sed -i 's/title_bar = "on"/title_bar = "negotiated"/' "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null
title_bar_wait_field ssd-x11 title_bar true
printf 'decorations off\n' >&"$control"
title_bar_wait_field ssd-x11 title_bar false
printf 'decorations on\n' >&"$control"
title_bar_wait_field ssd-x11 title_bar true
echo 'Xwayland title bars follow Motif hint transitions and rule overrides'
