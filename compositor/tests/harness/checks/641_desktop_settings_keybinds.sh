#!/usr/bin/env bash
# dsk_settings_manager_v1 keybinds: every built-in bind is reported in bind-request syntax, a new bind persists to
# settings.toml and applies, a second spelling of the same chord replaces the first instead of adding a duplicate,
# "none" unbinds a built-in chord, a malformed chord or action is refused, and an empty action restores the built-in.
set -euo pipefail

readonly DESKTOP=${UMBRIEL_DESKTOP_CLIENT:-./build-debug/tests/desktop-client}
readonly SAVED=$(dirname "$UMBRIEL_CONFIG")/settings.toml
trap 'rm -f "$SAVED"; "$UMBRIEL" msg config-reload > /dev/null' EXIT

bind_of() { "$DESKTOP" keybinds-state | grep "^$1=" || true; }
expect_bind() {
  local got=
  for _ in $(seq 40); do
    got=$(bind_of "${1%%=*}")
    [[ $got == "$2" ]] && return 0
    sleep 0.1
  done
  echo "expected '$2' for ${1%%=*}, got '$got'"
  return 1
}

[[ $(bind_of "Mod+q") == "Mod+q=window-close" ]] || { echo "built-in Mod+q not reported"; exit 1; }

"$DESKTOP" keybind-set "Mod+Shift+y" "spawn:foot"
expect_bind "Mod+Shift+y" "Mod+Shift+y=spawn:foot customized"
grep -q '"Mod+Shift+y" = "spawn:foot"' "$SAVED" || { echo "settings.toml lacks the bind:"; sed 's/^/  | /' "$SAVED"; exit 1; }

"$DESKTOP" keybind-set "mod+shift+Y" "window-close"
expect_bind "Mod+Shift+y" "Mod+Shift+y=window-close customized"
[[ $(grep -ci 'mod+shift+y' "$SAVED") == 1 ]] || { echo "two spellings of one chord saved"; exit 1; }

"$DESKTOP" keybind-set "Mod+q" "none"
expect_bind "Mod+q" "Mod+q=none customized"

if "$DESKTOP" keybind-set "Mod+Nope" "window-close" > /dev/null 2>&1; then
  echo "a malformed chord was accepted"
  exit 1
fi
if "$DESKTOP" keybind-set "Mod+Shift+u" "no-such-action" > /dev/null 2>&1; then
  echo "an unknown action was accepted"
  exit 1
fi

"$DESKTOP" keybind-set "Mod+q" ""
expect_bind "Mod+q" "Mod+q=window-close"

echo "keybinds report in bind syntax, persist, replace other spellings, unbind with none, and restore built-ins"
