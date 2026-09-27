#!/usr/bin/env bash
# Ending a mirror must not stack the output on its source. Two cases, both with displays.toml included:
# (1) HEADLESS-2 mirrors HEADLESS-1 and a settings app applies the layout as reported, where the mirror sits at 0,0:
#     displays.toml must not record that position for the mirror, and clearing the mirror must leave the two apart.
# (2) a displays.toml written before that rule, with both outputs at [0, 0] and the mirror key: clearing the mirror
#     must still place HEADLESS-2 beside HEADLESS-1. Writes position.txt, the displays.toml snapshots, and the
#     compositor log to $OUT (default ./artifacts/mirror-unmirror-position).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/mirror-unmirror-position}
source "$(dirname "$0")/lib.sh"
boot_headless 2
OM=$ROOT/compositor/build-debug/tests/output-management-client
DISPLAYS=$RUNTIME/displays.toml
: > "$OUT/position.txt"
result() { echo "$1" | tee -a "$OUT/position.txt"; }
position() { run "$UMBRIEL" outputs --json | jq -r ".[] | select(.name == \"$1\") | \"\(.position.x),\(.position.y)\""; }
# Headless outputs are all 1280x720 at scale 1.
apart() {
  local a b
  a=$(position HEADLESS-1) b=$(position HEADLESS-2)
  echo "HEADLESS-1 at $a, HEADLESS-2 at $b" >> "$OUT/position.txt"
  (( ${a%,*} - ${b%,*} >= 1280 || ${b%,*} - ${a%,*} >= 1280 || ${a#*,} - ${b#*,} >= 720 || ${b#*,} - ${a#*,} >= 720 ))
}
wait_log() {
  for _ in $(seq 50); do grep -q "$1" "$OUT/umbriel.log" && return 0; sleep 0.1; done
  return 1
}

sed -i 's/files = \["input.toml"\]/files = ["input.toml", "displays.toml"]/' "$RUNTIME/umbriel.toml"
run "$UMBRIEL" msg config-reload > /dev/null
run "$DESKTOP_CLIENT" mirror HEADLESS-2 HEADLESS-1
wait_log "output 'HEADLESS-2': mirroring 'HEADLESS-1'"
run "$OM" apply enable HEADLESS-1 > /dev/null
cp "$DISPLAYS" "$OUT/displays-after-apply.toml"
saved=$(awk '/^\[output.HEADLESS-2\]/{on=1; next} /^\[/{on=0} on && /^position/' "$DISPLAYS")
[[ -z $saved ]] && result "PASS apply does not persist the mirror's reported position" \
  || result "FAIL apply persisted the mirror's position: $saved"
run "$DESKTOP_CLIENT" clear HEADLESS-2
wait_log "output 'HEADLESS-2': stopped mirroring"
run "$UMBRIEL" settle > /dev/null
apart && result "PASS clearing the mirror leaves HEADLESS-2 beside HEADLESS-1" \
  || result "FAIL clearing the mirror stacked HEADLESS-2 on HEADLESS-1"

cat > "$DISPLAYS" <<'TOML'
[output.HEADLESS-1]
position = [0, 0]

[output.HEADLESS-2]
mirror = "HEADLESS-1"
position = [0, 0]
TOML
cp "$DISPLAYS" "$OUT/displays-stacked.toml"
from=$(wc -l < "$OUT/umbriel.log")
run "$UMBRIEL" msg config-reload > /dev/null
for _ in $(seq 50); do
  tail -n +"$((from + 1))" "$OUT/umbriel.log" | grep -q "output 'HEADLESS-2': mirroring 'HEADLESS-1'" && break
  sleep 0.1
done
from=$(wc -l < "$OUT/umbriel.log")
run "$DESKTOP_CLIENT" clear HEADLESS-2
for _ in $(seq 50); do
  tail -n +"$((from + 1))" "$OUT/umbriel.log" | grep -q "output 'HEADLESS-2': stopped mirroring" && break
  sleep 0.1
done
run "$UMBRIEL" settle > /dev/null
apart && result "PASS a stacked saved position is not reused when the mirror ends" \
  || result "FAIL a stacked saved position put HEADLESS-2 on HEADLESS-1"

! grep -q '^FAIL' "$OUT/position.txt" || { echo "FAIL; artifacts: $OUT"; exit 1; }
echo "PASS; artifacts: $OUT"
