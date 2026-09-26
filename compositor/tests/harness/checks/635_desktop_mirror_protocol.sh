#!/usr/bin/env bash
# harness: outputs=2
# dsk_output_manager_v1 starts and stops mirrors, broadcasts the state, saves it to displays.toml, rejects invalid
# requests, and an output-management disable of a mirror ends it.
set -euo pipefail

readonly DESKTOP=${UMBRIEL_DESKTOP_CLIENT:-./build-debug/tests/desktop-client}
readonly OUTPUT_MANAGEMENT=${UMBRIEL_OUTPUT_MANAGEMENT_CLIENT:-./build-debug/tests/output-management-client}
readonly SAVED=$(dirname "$UMBRIEL_CONFIG")/displays.toml

expect_state() {
  local expected=$1 state=
  for _ in $(seq 40); do
    state=$("$DESKTOP" state | grep '^HEADLESS-2 ' || true)
    [[ $state == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected mirror state '$expected', got '$state'"
  return 1
}

if reason=$("$DESKTOP" mirror HEADLESS-1 HEADLESS-1 2>&1); then
  echo "mirroring an output onto itself was accepted"
  exit 1
fi
[[ $reason == *"itself"* ]] || { echo "unexpected rejection reason: $reason"; exit 1; }
if "$DESKTOP" mirror HEADLESS-2 NOPE-9 > /dev/null 2>&1; then
  echo "mirroring an unknown output was accepted"
  exit 1
fi

"$DESKTOP" mirror HEADLESS-2 HEADLESS-1
expect_state "HEADLESS-2 HEADLESS-1"
if ! grep -A20 '^\[output.HEADLESS-2\]' "$SAVED" | grep -qE "^mirror = ['\"]HEADLESS-1['\"]"; then
  echo "displays.toml does not record the mirror:"
  sed 's/^/  | /' "$SAVED"
  exit 1
fi
if "$DESKTOP" mirror HEADLESS-1 HEADLESS-2 > /dev/null 2>&1; then
  echo "an output mirroring a mirror was accepted"
  exit 1
fi

"$DESKTOP" clear HEADLESS-2
expect_state "HEADLESS-2 -"
if grep -A20 '^\[output.HEADLESS-2\]' "$SAVED" | grep -q '^mirror ='; then
  echo "displays.toml still records a mirror after clear"
  exit 1
fi

"$DESKTOP" mirror HEADLESS-2 HEADLESS-1
expect_state "HEADLESS-2 HEADLESS-1"
"$OUTPUT_MANAGEMENT" apply disable HEADLESS-2 > /dev/null
expect_state "HEADLESS-2 -"
enabled=$("$UMBRIEL" outputs --json | jq -r '.[] | select(.name == "HEADLESS-2") | .enabled')
if [[ $enabled != false ]]; then
  echo "an output-management disable of a mirror left it enabled='$enabled'"
  exit 1
fi

echo "mirror protocol sets, broadcasts, saves, rejects, and yields to output-management disable"
