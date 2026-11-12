#!/usr/bin/env bash
# harness: outputs=2
# Per-key ownership of output settings: a wlr-output-management apply is written to displays.toml. Once config.toml
# gains an [output.<name>] table that sets only `enabled`, a later apply still takes effect live and its position is
# still persisted to displays.toml, because config.toml does not set that key.
set -euo pipefail

readonly OUTPUT_MANAGEMENT=${ANDROMEDA_OUTPUT_MANAGEMENT_CLIENT:-./build-debug/tests/output-management-client}
readonly SAVED=$(dirname "$ANDROMEDA_CONFIG")/displays.toml

position_of() {
  for _ in $(seq 40); do
    local p
    p=$("$ANDROMEDA" outputs --json | jq -r --arg n "$1" '.[] | select(.name == $n) | "\(.position.x),\(.position.y)"')
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

# config.toml now sets one key of HEADLESS-2.
{
  cat "$ANDROMEDA_CONFIG"
  printf '\n[output.HEADLESS-2]\nenabled = true\n'
} > "$ANDROMEDA_CONFIG.tmp"
mv "$ANDROMEDA_CONFIG.tmp" "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null

"$OUTPUT_MANAGEMENT" apply enable HEADLESS-2 30 40 > /dev/null

[[ $(position_of HEADLESS-2) == "30,40" ]] || { echo "the second apply did not take effect live"; exit 1; }

for _ in $(seq 40); do
  grep -qE '^position = \[ ?30, 40 ?\]' "$SAVED" && break
  sleep 0.1
done
if ! grep -qE '^position = \[ ?30, 40 ?\]' "$SAVED"; then
  echo "displays.toml did not save 30,40 though config.toml sets only enabled for HEADLESS-2:"
  sed 's/^/  | /' "$SAVED"
  exit 1
fi

echo "an apply persists the keys a config.toml [output] table does not set"
