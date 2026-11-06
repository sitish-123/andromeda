#!/usr/bin/env bash
# Settings › Per-Workspace Arrangement over dsk_settings_manager_v1: every workspace slot reports
# workspace.<N>.layout.mode (empty = the global arrangement); setting one switches only that workspace and saves a
# [[workspace]] rule; an empty value removes it; it also drops a session override from workspace-set-layout (ST2).
set -euo pipefail
readonly D=${ANDROMEDA_DESKTOP_CLIENT:-./build-debug/tests/desktop-client}
readonly SAVED=$(dirname "$ANDROMEDA_CONFIG")/settings.toml
trap 'rm -f "$SAVED"' EXIT

expect_setting() {
  local got=
  for _ in $(seq 40); do
    got=$("$D" settings-state | grep "^${1%%=*}=" || true)
    [[ $got == "$1" ]] && return 0
    sleep 0.1
  done
  echo "expected '$1', got '$got'"
  return 1
}
layout_of() { "$ANDROMEDA" workspaces --json | jq -r ".[] | select(.index == $1) | .layout"; }
expect_layout() {
  for _ in $(seq 40); do
    [[ $(layout_of "$1") == "$2" ]] && return 0
    sleep 0.1
  done
  echo "workspace $1 is $(layout_of "$1"), expected $2"
  return 1
}

cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'
[layout]
mode = "scrolling"
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null
# An empty dynamic workspace goes away when left, so workspace 2 keeps a window.
"$ANDROMEDA" msg workspace-switch:2 > /dev/null
"$ANDROMEDA_UNMAP_CLIENT" keep-two 300 200 > /dev/null 2>&1 &
for _ in $(seq 60); do
  [[ $("$ANDROMEDA" windows --json | jq 'length') -eq 1 ]] && break
  sleep 0.05
done
"$ANDROMEDA" msg workspace-switch:1 > /dev/null
expect_setting "workspace.1.layout.mode="
expect_setting "workspace.2.layout.mode="

"$D" settings-set workspace.2.layout.mode master
expect_setting "workspace.2.layout.mode=master customized"
expect_layout 2 master
expect_layout 1 scrolling
grep -q "mode = 'master'" "$SAVED" || { echo "the arrangement was not saved: $(cat "$SAVED")"; exit 1; }

# A saved choice replaces a session override on that workspace.
"$ANDROMEDA" msg workspace-set-layout:floating > /dev/null
expect_layout 1 floating
"$D" settings-set workspace.1.layout.mode dwindle
expect_layout 1 dwindle

"$D" settings-set workspace.2.layout.mode ""
expect_setting "workspace.2.layout.mode="
expect_layout 2 scrolling
if "$D" settings-set workspace.2.layout.mode grid 2> /dev/null; then
  expect_setting "workspace.2.layout.mode="
fi
echo "workspace slots report, save and clear their own arrangement"
