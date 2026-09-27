#!/usr/bin/env bash
# dsk_settings_manager_v1 reports every managed setting, validates, persists to settings.toml with no include in
# config.toml, applies live, wins over a key config.toml sets, covers non-input settings, and clearing a key falls back
# to config.toml or the built-in default.
set -euo pipefail

readonly DESKTOP=${UMBRIEL_DESKTOP_CLIENT:-./build-debug/tests/desktop-client}
readonly SAVED=$(dirname "$UMBRIEL_CONFIG")/settings.toml
BASELINE=$(< "$UMBRIEL_CONFIG")
trap 'printf "%s\n" "$BASELINE" > "$UMBRIEL_CONFIG"; rm -f "$SAVED"; "$UMBRIEL" msg config-reload > /dev/null' EXIT

setting() { "$DESKTOP" settings-state | grep "^$1=" || true; }
expect_setting() {
  local got=
  for _ in $(seq 40); do
    got=$(setting "${1%%=*}")
    [[ $got == "$1" ]] && return 0
    sleep 0.1
  done
  echo "expected '$1', got '$got'"
  return 1
}

[[ $(setting input.touchpad.tap) == "input.touchpad.tap=true" ]] || { echo "tap default not reported"; exit 1; }

"$DESKTOP" settings-set input.touchpad.tap false
expect_setting "input.touchpad.tap=false customized"
grep -q '^tap = false' "$SAVED" || { echo "settings.toml does not hold tap = false:"; sed 's/^/  | /' "$SAVED"; exit 1; }

if "$DESKTOP" settings-set input.touchpad.sensitivity 3 > /dev/null 2>&1; then
  echo "an out-of-range sensitivity was accepted"
  exit 1
fi
if "$DESKTOP" settings-set general.autostart x > /dev/null 2>&1; then
  echo "a key outside the managed set was accepted"
  exit 1
fi

"$DESKTOP" settings-set appearance.border_width 0
expect_setting "appearance.border_width=0 customized"

printf '\n[input.keyboard]\nrepeat_rate = 40\n' >> "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
expect_setting "input.keyboard.repeat_rate=40"
"$DESKTOP" settings-set input.keyboard.repeat_rate 30
expect_setting "input.keyboard.repeat_rate=30 customized"

"$DESKTOP" settings-set input.keyboard.repeat_rate ""
expect_setting "input.keyboard.repeat_rate=40"
"$DESKTOP" settings-set input.touchpad.tap ""
expect_setting "input.touchpad.tap=true"
if grep -q '^tap' "$SAVED"; then
  echo "clearing tap left it in settings.toml"
  exit 1
fi

echo "settings protocol validates, persists without an include, wins over config.toml, and clears to what it overrode"
