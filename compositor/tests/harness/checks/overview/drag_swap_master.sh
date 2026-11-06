#!/usr/bin/env bash
# Spaces overview, Main + Stack: swapping the master window with the lower stack window keeps one master window.
# Lifting the master promotes the first stack window; the swap must send it back to the stack.
set -euo pipefail
source "$(dirname "$ANDROMEDA_HARNESS_LIB")/title_bar.sh"
source "$ANDROMEDA_HARNESS_LIB"
cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'
[animation]
enabled = false
[appearance.title_bar]
mode = "off"
[layout]
mode = "master"
[overview]
style = "spaces"
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null
for title in main upper lower; do
  "$ANDROMEDA_FRACTIONAL_CLIENT" "$title" 480 300 > "$ANDROMEDA_RUNTIME_DIR/$title.log" 2>&1 &
  title_bar_wait_field "$title" floating false
done
"$ANDROMEDA" settle > /dev/null
field() { title_bar_window "$1" | jq -r ".$2"; }
# Roles by position: the master window is leftmost; the stack holds a top and a bottom window.
read -r main top bottom < <(for title in main upper lower; do echo "$(field "$title" x) $(field "$title" y) $title"; done |
  sort -n -k1,1 -k2,2 | awk '{ printf "%s ", $3 } END { print "" }')
x_main=$(field "$main" x)
(($(field "$top" x) > x_main && $(field "$top" y) < $(field "$bottom" y))) ||
  { echo "expected one master left of a two-window stack"; exit 1; }
"$ANDROMEDA" msg overview-open > /dev/null
"$ANDROMEDA" settle > /dev/null
# The main area spans x 212..1067, y 203..684. Lifting the master promotes the top window, so the bottom one fills
# the right half.
# The lift reflows the cards on the next frame; settle before moving onto the target so the drop resolves against
# the reflowed layout, not the stack the lift just left.
pointer_hold 1280 720 move 426 443 press 272 move 426 483 -- move 820 443 move 853 443 release 272
"$ANDROMEDA" settle > /dev/null
pointer_release
"$ANDROMEDA" settle > /dev/null
"$ANDROMEDA" msg overview-close > /dev/null
"$ANDROMEDA" settle > /dev/null
state="$bottom ($(field "$bottom" x),$(field "$bottom" y)) $top ($(field "$top" x),$(field "$top" y)) $main ($(field "$main" x),$(field "$main" y))"
(($(field "$bottom" x) == x_main && $(field "$top" x) > x_main && $(field "$main" x) > x_main)) ||
  { echo "expected $bottom alone in the master area: $state"; exit 1; }
(($(field "$top" y) < $(field "$main" y))) || { echo "expected $top above $main in the stack: $state"; exit 1; }
echo "swapping the master window kept one master window and the stack order"
