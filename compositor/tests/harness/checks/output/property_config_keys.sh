#!/usr/bin/env bash
# harness: outputs=1
# A config.toml [output] table owns only the keys it sets. Settings may still save its other display properties to
# displays.toml, but not a key config.toml sets, nor `workspaces` while config.toml sets its exclusive peer
# `min_workspaces`. A displays.toml that already holds such a peer loses it at load, so config.toml still loads.
set -euo pipefail

readonly DESKTOP=${ANDROMEDA_DESKTOP_CLIENT:-./build-debug/tests/desktop-client}
readonly DISPLAYS=$(dirname "$ANDROMEDA_CONFIG")/displays.toml
readonly BASE=$(cat "$ANDROMEDA_CONFIG")
trap 'rm -f "$DISPLAYS"' EXIT

output=$("$ANDROMEDA" workspaces --json | jq -r '.[0].output')
write_config() {
  printf '%s\n\n[include.optional]\nfiles = ["displays.toml"]\n\n[output.%s]\nmin_workspaces = %s\n' \
    "$BASE" "$output" "$1" > "$ANDROMEDA_CONFIG"
  "$ANDROMEDA" msg config-reload > /dev/null
}
prop_of() { "$DESKTOP" properties-state | grep "^$output $1=" | cut -d= -f2- || true; }
expect_prop() {
  local got=
  for _ in $(seq 40); do
    got=$(prop_of "$1")
    [[ $got == "$2" ]] && return 0
    sleep 0.1
  done
  echo "expected $1=$2 on $output, got '$got'"
  return 1
}

write_config 3
expect_prop min_workspaces 3

if "$DESKTOP" property-set "$output" min_workspaces 2 > /dev/null 2>&1; then
  echo "min_workspaces was accepted though config.toml sets it"
  exit 1
fi
if "$DESKTOP" property-set "$output" workspaces 4 > /dev/null 2>&1; then
  echo "workspaces=4 was accepted though config.toml sets min_workspaces, which needs dynamic workspaces"
  exit 1
fi
if grep -qs 'workspaces' "$DISPLAYS"; then
  echo "a refused key reached displays.toml:"
  sed 's/^/  | /' "$DISPLAYS"
  exit 1
fi

"$DESKTOP" property-set "$output" vrr fullscreen
expect_prop vrr fullscreen

# A displays.toml written before the refusal existed: config.toml's min_workspaces wins and the config still loads,
# so the new min_workspaces = 2 takes effect.
printf '[output.%s]\nworkspaces = 4\n' "$output" > "$DISPLAYS"
write_config 2
expect_prop min_workspaces 2
expect_prop workspaces dynamic

echo "config.toml keys and their exclusive peers are refused; a conflicting saved peer is dropped at load"
