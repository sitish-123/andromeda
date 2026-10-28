#!/usr/bin/env bash
# A window mapped into the outgoing workspace during a slide must be hidden when the slide finishes.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

cat >> "$ANDROMEDA_CONFIG" <<'EOF'
[animation.workspaces]
duration_ms = 1000
curve = "linear"

[colors]
backdrop = "#000000FF"

[output."HEADLESS-1"]
workspaces = 2

[[window_rule]]
match.app_id = "^late-map$"
default_workspace = 1
default_focused = false
EOF
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA" clock-freeze
"$ANDROMEDA" msg workspace-switch:2 > /dev/null
"$ANDROMEDA" workspaces --json | jq -e '.[] | select(.name == "2" and .active)' > /dev/null
APP_ID=late-map "$ANDROMEDA_UNMAP_CLIENT" late-map 1200 700 > "$ANDROMEDA_RUNTIME_DIR/client.log" 2>&1 &
await_lines "$ANDROMEDA_RUNTIME_DIR/client.log" 'mapped' 1
"$ANDROMEDA" clock-advance 1200
"$ANDROMEDA" clock-resume
"$ANDROMEDA" settle
grim "$ANDROMEDA_RUNTIME_DIR/late-map.png"
read -r _ _ blue < <("$ANDROMEDA_PIXEL_PROBE" "$ANDROMEDA_RUNTIME_DIR/late-map.png" mean)
if ((blue != 0)); then
  echo "inactive late-mapped window remains visible: mean blue=$blue"
  exit 1
fi
