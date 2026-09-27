#!/usr/bin/env bash
# A foot terminal printing a counter on the source output while a 1.5x output at the same origin mirrors it, like a
# laptop panel mirroring a TV. The mirror target must leave the scene: otherwise wlroots keeps it among the window's
# outputs, hands the window the mirror's scale, and paces its frame callbacks by the mirror, which never sends them
# (a 120 Hz panel wins that pacing over a 60 Hz TV; headless outputs report no refresh, so the scale is the probe).
# Asserts the window is never offered the mirror's scale once the mirror settles and that the source keeps committing
# (frames.txt, wayland-debug.log, source.png). Artifacts go to $OUT (default ./artifacts/mirror-frames).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/mirror-frames}
source "$(dirname "$0")/lib.sh"
boot_headless 2

commits() { run "$UMBRIEL" output-commits --json | jq '."HEADLESS-1"'; }

: > "$OUT/frames.txt"
printf '\n[output.HEADLESS-2]\nscale = 1.5\nposition = [0, 0]\n' >> "$RUNTIME/umbriel.toml"
for _ in $(seq 50); do
  [[ $(run "$UMBRIEL" outputs --json | jq '.[] | select(.name == "HEADLESS-2") | .position.x') == 0 ]] && break
  sleep 0.1
done
run env WAYLAND_DEBUG=1 foot --config=/dev/null \
  sh -c 'i=0; while :; do i=$((i+1)); printf "\r%08d" "$i"; sleep 0.05; done' 2> "$OUT/wayland-debug.log" &
sleep 2 # real time: foot maps and starts counting
run "$DESKTOP_CLIENT" mirror HEADLESS-2 HEADLESS-1
sleep 1 # real time: views move to the source and outputs update
run "$DESKTOP_CLIENT" state >> "$OUT/frames.txt"
settled=$(wc -l < "$OUT/wayland-debug.log")

before=$(commits)
sleep 1.5 # real time: the counter advances ~30 times
after=$(commits)
scales=$(tail -n +"$settled" "$OUT/wayland-debug.log" | { grep -oP 'preferred_scale\(\K[0-9]+' || true; } | sort -u | paste -sd,)
last=$(grep -oP 'preferred_scale\(\K[0-9]+' "$OUT/wayland-debug.log" | tail -1)
echo "source commits in 1.5 s: $((after - before)); preferred_scale after settling: [${scales}] last: $last (120 = 1x)" \
  | tee -a "$OUT/frames.txt"
run grim -o HEADLESS-1 "$OUT/source.png"
(( after - before > 10 )) || { echo "FAIL: source window stopped redrawing; artifacts: $OUT"; exit 1; }
[[ $last == 120 && ( -z $scales || $scales == 120 ) ]] \
  || { echo "FAIL: window on the 1x source was given the mirror's scale; artifacts: $OUT"; exit 1; }
echo "PASS; artifacts: $OUT"
