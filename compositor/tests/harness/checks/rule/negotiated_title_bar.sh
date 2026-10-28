#!/usr/bin/env bash
# Negotiated mode requires Wayland decoration negotiation and follows live protocol changes.
set -euo pipefail
source "$(dirname "$ANDROMEDA_HARNESS_LIB")/title_bar.sh"
title_bar_config
cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'
[[window_rule]]
match.title = "^negotiated-rule$"
title_bar = "negotiated"
[[window_rule]]
match.title = "^negotiated-on$"
title_bar = "on"
[[window_rule]]
match.title = "^negotiated-forced$"
title_bar = "forced"
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null

declare -A controls
start_controlled() {
  local title=$1 fd
  shift
  mkfifo "$ANDROMEDA_RUNTIME_DIR/$title.fifo"
  exec {fd}<>"$ANDROMEDA_RUNTIME_DIR/$title.fifo"
  controls[$title]=$fd
  env DECORATION_CONTROL=1 LOG_CONFIGURES=1 "$@" "$ANDROMEDA_FRACTIONAL_CLIENT" "$title" 480 300 \
    <&"$fd" > "$ANDROMEDA_RUNTIME_DIR/$title.log" 2>&1 &
}
command_to() {
  printf '%s\n' "$2" >&"${controls[$1]}"
}
set_mode() {
  sed -i '/^\[appearance.title_bar\]$/ { n; s/mode = "[^"]*"/mode = "'"$1"'"/; }' "$ANDROMEDA_CONFIG"
  "$ANDROMEDA" msg config-reload > /dev/null
}
initial_height() {
  local first
  first=$(rg -m 1 '^configure ' "$ANDROMEDA_RUNTIME_DIR/$1.log")
  [[ ${first##*x} == "$2" ]] || { echo "$1: expected initial height $2, got $first"; return 1; }
}

# A live switch from on removes the bar and returns its space to an unnegotiated client.
start_controlled negotiated-inherit
title_bar_wait_field negotiated-inherit title_bar true
initial_height negotiated-inherit 676
"$ANDROMEDA" settle > /dev/null
before=$(title_bar_window negotiated-inherit)
pixel_x=$(($(jq -r .x <<< "$before") + 5))
pixel_y=$(($(jq -r .y <<< "$before") - 14))
image=$ANDROMEDA_RUNTIME_DIR/negotiated.png
grim -o HEADLESS-1 "$image"
read -r red green blue < <("$ANDROMEDA_PIXEL_PROBE" "$image" pixel "$pixel_x" "$pixel_y")
((red > 240 && green < 12 && blue > 240))
set_mode negotiated
title_bar_wait_field negotiated-inherit title_bar false
"$ANDROMEDA" settle > /dev/null
after=$(title_bar_window negotiated-inherit)
[[ $(jq -r .h <<< "$after") == $(($(jq -r .h <<< "$before") + 28)) ]]
[[ $(jq -r .y <<< "$after") == $(($(jq -r .y <<< "$before") - 28)) ]]
[[ $(jq -c '[.x, .w]' <<< "$after") == "$(jq -c '[.x, .w]' <<< "$before")" ]]
grim -o HEADLESS-1 "$image"
read -r red _ _ < <("$ANDROMEDA_PIXEL_PROBE" "$image" pixel "$pixel_x" "$pixel_y")
((red < 12))
start_controlled negotiated-late
title_bar_wait_field negotiated-late title_bar false
initial_height negotiated-late 704

# Creating the xdg decoration object without requesting a mode still negotiates SSD.
start_controlled negotiated-default XDG_DECORATION=unset
title_bar_wait_field negotiated-default title_bar true
initial_height negotiated-default 676
start_controlled negotiated-xdg XDG_DECORATION=server
title_bar_wait_field negotiated-xdg title_bar true
command_to negotiated-xdg 'xdg client'
title_bar_wait_field negotiated-xdg title_bar false
command_to negotiated-xdg 'xdg unset'
title_bar_wait_field negotiated-xdg title_bar true
command_to negotiated-xdg 'xdg destroy'
title_bar_wait_field negotiated-xdg title_bar false

start_controlled negotiated-legacy LEGACY_DECORATION=server
title_bar_wait_field negotiated-legacy title_bar true
command_to negotiated-legacy 'legacy none'
title_bar_wait_field negotiated-legacy title_bar false
command_to negotiated-legacy 'legacy server'
title_bar_wait_field negotiated-legacy title_bar true
command_to negotiated-legacy 'legacy destroy'
title_bar_wait_field negotiated-legacy title_bar false

# Xdg preference wins until its object is destroyed, then legacy takes over.
start_controlled negotiated-both LEGACY_DECORATION=server XDG_DECORATION=client
title_bar_wait_field negotiated-both title_bar false
command_to negotiated-both 'legacy client'
command_to negotiated-both 'legacy server'
title_bar_wait_field negotiated-both title_bar false
command_to negotiated-both 'xdg server'
title_bar_wait_field negotiated-both title_bar true
command_to negotiated-both 'xdg client'
title_bar_wait_field negotiated-both title_bar false
command_to negotiated-both 'xdg destroy'
title_bar_wait_field negotiated-both title_bar true
command_to negotiated-both 'legacy destroy'
title_bar_wait_field negotiated-both title_bar false

# Explicit rules keep their own policy across every global default.
start_controlled negotiated-on
title_bar_wait_field negotiated-on title_bar true
start_controlled negotiated-forced XDG_DECORATION=client
title_bar_wait_field negotiated-forced title_bar true
start_controlled negotiated-rule
title_bar_wait_field negotiated-rule title_bar false
for mode in on forced off negotiated; do
  set_mode "$mode"
  title_bar_wait_field negotiated-rule title_bar false
  title_bar_wait_field negotiated-on title_bar true
  title_bar_wait_field negotiated-forced title_bar true
done

# A later matching negotiated rule replaces forced mode on an existing CSD window.
cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'
[[window_rule]]
match.title = "^negotiated-forced$"
title_bar = "negotiated"
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null
title_bar_wait_field negotiated-forced title_bar false
command_to negotiated-forced 'xdg server'
title_bar_wait_field negotiated-forced title_bar true
command_to negotiated-forced 'xdg destroy'
title_bar_wait_field negotiated-forced title_bar false
echo 'negotiated title bars follow protocol transitions, opening sizes, global reloads, and explicit rules'
