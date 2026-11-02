#!/usr/bin/env bash
# dsk_settings_manager_v1 version 2: a layer value wins over the user's value and is never persisted; its client's
# disconnect restores the user's value and notifies subscribers; a volatile key is applied but never persisted; a
# version 1 client keeps working.
set -euo pipefail
readonly D=${ANDROMEDA_DESKTOP_CLIENT:-./build-debug/tests/desktop-client}
readonly SAVED=$(dirname "$ANDROMEDA_CONFIG")/settings.toml
readonly LOG=$(mktemp -d)
HOLDER=
WATCHER=
cleanup() {
  [[ -n $HOLDER ]] && kill "$HOLDER" 2> /dev/null || true
  [[ -n $WATCHER ]] && kill "$WATCHER" 2> /dev/null || true
  rm -rf "$LOG" "$SAVED"
}
trap cleanup EXIT

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
expect_notified() { # the watcher's last report of the key
  local got=
  for _ in $(seq 40); do
    got=$(grep "^${1%%=*}=" "$LOG/watch" | tail -n 1 || true)
    [[ $got == "$1" ]] && return 0
    sleep 0.1
  done
  echo "watcher expected '$1', last saw '$got'"
  return 1
}
never_saved() {
  if [[ -f $SAVED ]] && grep -q "$1" "$SAVED"; then
    echo "settings.toml holds $1:"
    sed 's/^/  | /' "$SAVED"
    return 1
  fi
}

"$D" settings-watch > "$LOG/watch" &
WATCHER=$!

"$D" settings-set appearance.border_width 3
expect_setting "appearance.border_width=3 customized"

"$D" layer-hold focus appearance.border_width 7 > "$LOG/hold" &
HOLDER=$!
for _ in $(seq 40); do
  grep -q '^ready' "$LOG/hold" && break
  sleep 0.1
done
expect_setting "appearance.border_width=7 customized"
expect_notified "appearance.border_width=7 customized"
never_saved '= 7'
grep -q '^border_width = 3' "$SAVED"

kill "$HOLDER"
wait "$HOLDER" 2> /dev/null || true
HOLDER=
expect_setting "appearance.border_width=3 customized"
expect_notified "appearance.border_width=3 customized"
never_saved '= 7'

expect_setting "power.on_battery=false"
"$D" settings-set power.on_battery true
expect_setting "power.on_battery=true"
expect_notified "power.on_battery=true"
never_saved on_battery

if "$D" layer-hold focus power.on_battery true > /dev/null 2>&1; then
  echo "a volatile key was accepted in a layer"
  exit 1
fi

DESKTOP_CLIENT_SETTINGS_VERSION=1 "$D" settings-set input.touchpad.tap false
expect_setting "input.touchpad.tap=false customized"
if DESKTOP_CLIENT_SETTINGS_VERSION=1 "$D" settings-state | grep -q '^power\.'; then
  echo "a version 1 client was sent a volatile key"
  exit 1
fi

dumps() { grep -c '^appearance.border_width=' "$LOG/watch" || true; }
mkfifo "$LOG/in"
DESKTOP_CLIENT_LAYER_REPEAT=1 "$D" layer-hold focus appearance.border_width 9 < "$LOG/in" > "$LOG/hold2" &
HOLDER=$!
exec 7> "$LOG/in"
for _ in $(seq 40); do grep -q '^ready' "$LOG/hold2" && break; sleep 0.1; done
expect_notified "appearance.border_width=9 customized"
before=$(dumps)
echo > /dev/fd/7
for _ in $(seq 40); do grep -q '^repeated' "$LOG/hold2" && break; sleep 0.1; done
expect_setting "appearance.border_width=9 customized"
[[ $(dumps) -eq $before ]] || { echo "an unchanged layer value caused another settings dump"; exit 1; }
exec 7>&-
kill "$HOLDER"; wait "$HOLDER" 2> /dev/null || true
HOLDER=
expect_notified "appearance.border_width=3 customized"

# An empty value never leaves a layer behind.
"$D" layer-hold focus appearance.border_width "" > "$LOG/hold4" &
HOLDER=$!
for _ in $(seq 40); do grep -q '^ready' "$LOG/hold4" && break; sleep 0.1; done
expect_setting "appearance.border_width=3 customized"
kill "$HOLDER"; wait "$HOLDER" 2> /dev/null || true
HOLDER=

if DESKTOP_CLIENT_SETTINGS_VERSION=1 "$D" settings-set power.on_battery true > /dev/null 2>&1; then
  echo "a version 1 client set a volatile key"
  exit 1
fi
