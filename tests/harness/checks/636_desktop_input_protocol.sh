#!/usr/bin/env bash
# dsk_input_manager_v1 reports input settings, refuses changes until config.toml includes input.toml, validates,
# persists, applies live, reports keys config.toml sets as locked and refuses them, and clears a key to its default.
set -euo pipefail

readonly DESKTOP=${UMBRIEL_DESKTOP_CLIENT:-./build-debug/tests/desktop-client}
readonly SAVED=$(dirname "$UMBRIEL_CONFIG")/input.toml
BASELINE=$(< "$UMBRIEL_CONFIG")

setting() { "$DESKTOP" input-state | grep "^$1=" || true; }
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

if reason=$("$DESKTOP" input-set input.touchpad.tap false 2>&1); then
  echo "a change was accepted while config.toml does not include input.toml"
  exit 1
fi
[[ $reason == *"does not include input.toml"* ]] || { echo "unexpected reason: $reason"; exit 1; }

{
  printf '[include.optional]\nfiles = ["input.toml"]\n\n'
  printf '%s\n' "$BASELINE"
} > "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null

"$DESKTOP" input-set input.touchpad.tap false
expect_setting "input.touchpad.tap=false"
grep -q '^tap = false' "$SAVED" || { echo "input.toml does not hold tap = false:"; sed 's/^/  | /' "$SAVED"; exit 1; }

if "$DESKTOP" input-set input.touchpad.sensitivity 3 > /dev/null 2>&1; then
  echo "an out-of-range sensitivity was accepted"
  exit 1
fi
if "$DESKTOP" input-set general.autostart x > /dev/null 2>&1; then
  echo "a non-input key was accepted"
  exit 1
fi

printf '\n[input.keyboard]\nrepeat_rate = 40\n' >> "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
expect_setting "input.keyboard.repeat_rate=40 locked"
if reason=$("$DESKTOP" input-set input.keyboard.repeat_rate 30 2>&1); then
  echo "a key config.toml sets was changed"
  exit 1
fi
[[ $reason == *"overrides"* ]] || { echo "unexpected reason: $reason"; exit 1; }

"$DESKTOP" input-set input.touchpad.tap ""
expect_setting "input.touchpad.tap=true"
if grep -q '^tap' "$SAVED"; then
  echo "clearing tap left it in input.toml"
  exit 1
fi

echo "input protocol validates, persists, applies, locks config.toml keys, and clears to defaults"
