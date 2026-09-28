#!/usr/bin/env bash
# Hot corners from a settings app: the hot_corners.top_left keys set through dsk_settings_manager_v1 persist to
# settings.toml and make a dwell in the corner run the action; clearing the keys back to config.toml turns the
# corner off again.
set -euo pipefail
readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly DESKTOP=${UMBRIEL_DESKTOP_CLIENT:-./build-debug/tests/desktop-client}
readonly SAVED=$(dirname "$UMBRIEL_CONFIG")/settings.toml
trap 'rm -f "$SAVED"; "$UMBRIEL" msg config-reload > /dev/null' EXIT

pointer() { "$POINTER" "$OUTPUT_W" "$OUTPUT_H" "$@"; }
active_workspace() { "$UMBRIEL" workspaces --json | jq -r '.[] | select(.output == "HEADLESS-1" and .active) | .index'; }
setting_is() {
  for _ in $(seq 40); do
    [[ $("$DESKTOP" settings-state | grep "^$1=" | cut -d= -f2- | sed 's/ customized$//') == "$2" ]] && return 0
    sleep 0.1
  done
  echo "$1 never became $2: $("$DESKTOP" settings-state | grep "^$1=")"
  return 1
}

pointer move 640 360
cat >> "$UMBRIEL_CONFIG" <<'CONF'
[output.HEADLESS-1]
workspaces = 2
[animation]
enabled = false
[input.focus]
follows_mouse = false
CONF
"$UMBRIEL" msg config-reload > /dev/null

"$DESKTOP" settings-set hot_corners.top_left.action "workspace-switch:2/HEADLESS-1"
"$DESKTOP" settings-set hot_corners.top_left.delay_ms 100
"$DESKTOP" settings-set hot_corners.top_left.enabled true
setting_is hot_corners.top_left.enabled true
grep -q "top_left" "$SAVED" || { echo "settings.toml lacks the corner:"; sed 's/^/  | /' "$SAVED"; exit 1; }

pointer move 0 0 pause 300
[[ $(active_workspace) == 2 ]] || { echo "the managed hot corner did not fire (workspace $(active_workspace))"; exit 1; }

pointer move 640 360
"$UMBRIEL" msg workspace-switch:1 > /dev/null
for key in enabled delay_ms action; do "$DESKTOP" settings-set "hot_corners.top_left.$key" ""; done
setting_is hot_corners.top_left.enabled false
pointer move 0 0 pause 300
[[ $(active_workspace) == 1 ]] || { echo "the corner still fired after its settings were cleared"; exit 1; }

echo "a hot corner set from settings persisted, fired on a dwell, and stopped once cleared"
