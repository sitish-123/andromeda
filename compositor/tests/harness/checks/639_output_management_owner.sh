#!/usr/bin/env bash
# harness: outputs=2
# One owner for output settings (gap 0.11): a wlr-output-management apply for an output config.toml
# does not mention is written to displays.toml as usual. Once config.toml gains an [output.<name>] table for that
# output, a later apply still takes effect live, but is no longer persisted to displays.toml, and the reason is
# logged.
set -euo pipefail

readonly OUTPUT_MANAGEMENT=${UMBRIEL_OUTPUT_MANAGEMENT_CLIENT:-./build-debug/tests/output-management-client}
readonly SAVED=$(dirname "$UMBRIEL_CONFIG")/displays.toml

log_mark() { wc -l < "$UMBRIEL_LOG"; }
log_since() { tail -n +"$1" "$UMBRIEL_LOG"; }

position_of() {
  for _ in $(seq 40); do
    local p
    p=$("$UMBRIEL" outputs --json | jq -r --arg n "$1" '.[] | select(.name == $n) | "\(.position.x),\(.position.y)"')
    [[ -n $p ]] && { echo "$p"; return 0; }
    sleep 0.1
  done
  echo ""
}

# First apply: config.toml has no [output.HEADLESS-2] yet, so the save proceeds as usual.
"$OUTPUT_MANAGEMENT" apply enable HEADLESS-2 10 20 > /dev/null
for _ in $(seq 40); do
  [[ -f $SAVED ]] && grep -q '^\[output.HEADLESS-2\]' "$SAVED" && break
  sleep 0.1
done
grep -q '^\[output.HEADLESS-2\]' "$SAVED" || { echo "no [output.HEADLESS-2] in displays.toml after the first apply"; exit 1; }
if ! grep -qE '^position = \[ ?10, 20 ?\]' "$SAVED"; then
  echo "displays.toml does not hold HEADLESS-2 at 10,20:"
  sed 's/^/  | /' "$SAVED"
  exit 1
fi
[[ $(position_of HEADLESS-2) == "10,20" ]] || { echo "live position is not 10,20 after the first apply"; exit 1; }

# config.toml now claims HEADLESS-2.
{
  cat "$UMBRIEL_CONFIG"
  printf '\n[output.HEADLESS-2]\nenabled = true\n'
} > "$UMBRIEL_CONFIG.tmp"
mv "$UMBRIEL_CONFIG.tmp" "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null

mark=$(log_mark)
"$OUTPUT_MANAGEMENT" apply enable HEADLESS-2 30 40 > /dev/null

[[ $(position_of HEADLESS-2) == "30,40" ]] || { echo "a config.toml-owned output did not apply live"; exit 1; }

for _ in $(seq 40); do
  log_since "$mark" | grep -q "overrides changes made here" && break
  sleep 0.1
done
log_since "$mark" | grep -q "overrides changes made here" \
  || { echo "no ownership warning logged for the second apply"; exit 1; }

sleep 0.3 # real time: give a wrongly-persisted write a chance to land before checking its absence
if grep -qE '^position = \[ ?30, 40 ?\]' "$SAVED"; then
  echo "displays.toml was updated to 30,40 even though config.toml owns HEADLESS-2:"
  sed 's/^/  | /' "$SAVED"
  exit 1
fi
grep -qE '^position = \[ ?10, 20 ?\]' "$SAVED" \
  || { echo "displays.toml lost its pre-ownership position:"; sed 's/^/  | /' "$SAVED"; exit 1; }

echo "output-management applies live under config.toml ownership, and displays.toml keeps its pre-ownership value"
