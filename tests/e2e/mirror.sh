#!/usr/bin/env bash
# Runs the Umbriel fork nested in the current session with two outputs, WL-2 mirroring WL-1, and a green foot window.
# Captures both nested output windows from the host and asserts the mirror shows the source's pixels. Needs a running
# host Wayland session. Artifacts go to $OUT (default ./artifacts/mirror).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)

UMBRIEL=${UMBRIEL:-$ROOT/compositor/build-debug/umbriel}
PROBE=${PROBE:-$ROOT/compositor/build-debug/tests/pixel-probe}
OUT=${OUT:-$(pwd)/artifacts/mirror}
GREEN='g > 0.9 && r < 0.1 && b < 0.1'
mkdir -p "$OUT"
CONFIG=$(mktemp /tmp/dsk-mirror.XXXX.toml)
trap 'kill "$NESTED" 2>/dev/null; wait 2>/dev/null; rm -f "$CONFIG"' EXIT

cat > "$CONFIG" <<'CONF'
[general]
autostart = []
show_cheatsheet = false

[output.WL-2]
mirror = "WL-1"
CONF

before=$(umbriel windows --json | jq '[.[].id]')
WLR_WL_OUTPUTS=2 "$UMBRIEL" -c "$CONFIG" -s 'foot --config=/dev/null --override=colors-dark.background=00ff00' \
  > "$OUT/umbriel.log" 2>&1 &
NESTED=$!

geometry() {
  umbriel windows --json | jq -r --argjson before "$before" --arg title "$1" \
    '.[] | select((.id as $id | $before | index($id) | not) and (.title | endswith($title))) | "\(.x),\(.y) \(.w)x\(.h)"'
}
for _ in $(seq 50); do
  [[ -n $(geometry WL-1) && -n $(geometry WL-2) ]] && break
  sleep 0.1
done
sleep 1.5 # real time: let foot map and draw inside the nested compositor
grim -g "$(geometry WL-1)" "$OUT/source.png"
grim -g "$(geometry WL-2)" "$OUT/mirror.png"

source_green=$("$PROBE" "$OUT/source.png" count "$GREEN")
mirror_green=$("$PROBE" "$OUT/mirror.png" count "$GREEN")
printf 'source_green=%s mirror_green=%s\n' "$source_green" "$mirror_green" | tee "$OUT/result.txt"
(( source_green > 10000 )) || { echo "FAIL source window not green" | tee -a "$OUT/result.txt"; exit 1; }
(( mirror_green * 100 >= source_green * 95 )) || { echo "FAIL mirror does not match source" | tee -a "$OUT/result.txt"; exit 1; }
echo "PASS mirror shows the source" | tee -a "$OUT/result.txt"
