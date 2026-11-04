#!/usr/bin/env bash
# The accessibility settings change the live config: increase contrast raises a thin border to 2 and reduce
# transparency turns blur off, both reported by dsk_settings_manager_v1.
set -euo pipefail
readonly D=${ANDROMEDA_DESKTOP_CLIENT:-./build-debug/tests/desktop-client}
trap 'rm -f "$(dirname "$ANDROMEDA_CONFIG")/settings.toml"' EXIT

expect_setting() {
  local got=
  for _ in $(seq 40); do
    got=$("$D" settings-state | grep "^${1%%=*}=" || true)
    [[ $got == "$1"* ]] && return 0
    sleep 0.1
  done
  echo "expected '$1', got '$got'"
  return 1
}

"$D" settings-set appearance.border_width 0
expect_setting "appearance.border_width=0"
expect_setting "appearance.blur.enabled=true"
"$D" settings-set appearance.increase_contrast true
"$D" settings-set appearance.reduce_transparency true
expect_setting "appearance.border_width=2"
expect_setting "appearance.blur.enabled=false"
