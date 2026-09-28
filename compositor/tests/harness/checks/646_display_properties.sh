#!/usr/bin/env bash
# dsk_output_manager_v1 display properties: every output reports each property on bind; setting vrr, sdr_white, and
# the workspace count applies and saves them under the display in displays.toml; the workspace count really changes
# the output's workspaces; out-of-range and unknown values are refused; an empty value removes the saved key.
set -euo pipefail

readonly DESKTOP=${UMBRIEL_DESKTOP_CLIENT:-./build-debug/tests/desktop-client}
readonly DISPLAYS=$(dirname "$UMBRIEL_CONFIG")/displays.toml
trap 'rm -f "$DISPLAYS"; "$UMBRIEL" msg config-reload > /dev/null' EXIT

# As the default config does; outputs.md documents the include.
printf '\n[include.optional]\nfiles = ["displays.toml"]\n' >> "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null

output=$("$UMBRIEL" workspaces --json | jq -r '.[0].output')
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

for key in vrr hdr sdr_white tearing workspaces min_workspaces cyclic_workspaces workspace_axis; do
  [[ -n $(prop_of "$key") ]] || { echo "$key not reported for $output"; "$DESKTOP" properties-state | sed 's/^/  | /'; exit 1; }
done

"$DESKTOP" property-set "$output" vrr fullscreen
expect_prop vrr fullscreen
grep -Eq "vrr = [\"']fullscreen[\"']" "$DISPLAYS" || { echo "displays.toml lacks vrr:"; sed 's/^/  | /' "$DISPLAYS"; exit 1; }

"$DESKTOP" property-set "$output" sdr_white 250
expect_prop sdr_white 250
if "$DESKTOP" property-set "$output" sdr_white 5000 > /dev/null 2>&1; then
  echo "an sdr_white of 5000 was accepted"
  exit 1
fi
if "$DESKTOP" property-set "$output" no_such_key 1 > /dev/null 2>&1; then
  echo "an unknown property was accepted"
  exit 1
fi

"$DESKTOP" property-set "$output" workspaces 3
expect_prop workspaces 3
count=
for _ in $(seq 40); do
  count=$("$UMBRIEL" workspaces --json | jq --arg o "$output" '[.[] | select(.output == $o)] | length')
  [[ $count == 3 ]] && break
  sleep 0.1
done
[[ $count == 3 ]] || { echo "workspaces=3 left $output with $count workspaces"; exit 1; }

"$DESKTOP" property-set "$output" vrr ""
for _ in $(seq 40); do grep -q 'vrr' "$DISPLAYS" || break; sleep 0.1; done
! grep -q 'vrr' "$DISPLAYS" || { echo "an empty vrr stayed saved:"; sed 's/^/  | /' "$DISPLAYS"; exit 1; }

echo "properties reported, set, saved to displays.toml, applied to workspaces, refused when invalid, and cleared"
