#!/usr/bin/env bash
# Greeter mode draws the synced lock look (GS4/GS5): a sync.toml with [lockscreen] layout = "glance" and the bold clock
# puts the clock at the left (Glance), not in the centre; theme = "ryoku" draws the Ryoku scene. Writes glance.png,
# ryoku.png and greeter-*.log to $OUT. STYLE=<clock style id>, MATERIAL=<solid|tint|glass> and WALLPAPER=<image> with SHOT=1
# show another look (no checks).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/greeter-lock-look}
source "$(dirname "$0")/lib.sh"
rm -rf "$OUT"
boot_headless 1
fail() { echo "FAIL: $*" >&2; exit 1; }
wait_for() { local what=$1; shift; for _ in $(seq 100); do "$@" && return; sleep 0.1; done; run grim "$OUT/fail.png" || true; fail "$what"; }
# Mean brightness of a region "x,y wxh" of $2 (a PPM read with grim); over 8 = something light is drawn there.
lit() { local w=${1#* } ; w=${w%x*}; local h=${1#*x}; run grim -g "$1" -t ppm - | tail -c $((w * h * 3)) | od -An -tu1 -v \
  | awk '{for(i=1;i<=NF;i++){s+=$i;n++}}END{exit !(s/n > 8)}'; }

sock=$RUNTIME/greetd.sock
MOCK_SCENARIOS=plain,plain,plain,plain python3 -I "$ROOT/tests/e2e/mock_greetd.py" "$sock" "$OUT/requests.jsonl" > "$OUT/mock.log" 2>&1 &
wait_for "the mock did not start" grep -q ready "$OUT/mock.log"
mkdir -p "$RUNTIME/share/wayland-sessions" "$RUNTIME/greeter-state"
cp "$ROOT/compositor/data/andromeda.desktop" "$RUNTIME/share/wayland-sessions/"

greet() {
  local name=$1 theme=$2
  printf '[lockscreen]\ntheme = "%s"\nlayout = "glance"\nclock_style = "%s"\nclock_size = "l"\nclock_material = "%s"\n' "$theme" "${STYLE:-bold}" "${MATERIAL:-solid}" \
    > "$RUNTIME/greeter-state/sync.toml"
  [[ -z ${WALLPAPER:-} ]] || printf '[appearance.wallpaper]\npath = "%s"\n' "$WALLPAPER" >> "$RUNTIME/greeter-state/sync.toml"
  run env XDG_DATA_DIRS="$RUNTIME/share" NOCTALIA_GREETER_STATE_DIR="$RUNTIME/greeter-state" GREETD_SOCK="$sock" \
    "$NOCTALIA" greeter --user "$USER" > "$OUT/greeter-$name.log" 2>&1 &
  greeter=$!
  wait_for "no first frame ($name)" grep -q 'first frame at' "$OUT/greeter-$name.log"
  sleep 1
  run grim "$OUT/$name.png"
}

greet glance andromeda
if [[ ${SHOT:-0} != 1 ]]; then
  lit "${GLANCE_CLOCK:-120,160 200x60}" || fail "Glance: no clock at the left"
  lit "${CENTRE_CLOCK:-540,180 200x70}" && fail "Glance: the clock is still in the centre (default layout)"
fi
kill "$greeter"; wait "$greeter" 2> /dev/null || true

greet ryoku ryoku
[[ ${SHOT:-0} == 1 ]] || lit "${RYOKU_DIAL:-80,320 120x70}" || fail "Ryoku: no hour digits at the left of the dial"
kill "$greeter"; wait "$greeter" 2> /dev/null || true
echo "pass greeter_lock_look"
