#!/usr/bin/env bash
# At scale 1.5, the center island's media waveform stays in its row across workspace switches and after the media hover face
# collapses back to rest: with a test MPRIS player playing,
# the bars' centre row (they grow around it; median of 15 samples) is the same, within 1 px, before and after a
# switch to the next and back to the previous workspace, and after hovering the island and leaving it. Writes bars-*.txt and the crops to $OUT (default ./artifacts/island-bars-workspace).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/island-bars-workspace}
source "$(dirname "$0")/lib.sh"
rm -rf "$OUT"
boot_headless 1
# Fractional scale, as on a 2880x1800 laptop panel at 1.5: the 1280x720 output is 853x480 logical.
printf '\n[output.HEADLESS-1]\nscale = 1.5\n' >> "$RUNTIME/andromeda.toml"
run "$ANDROMEDA" msg config-reload > /dev/null

with_noctalia '
  fail() { echo "FAIL: $*" >&2; exit 1; }
  # Centre row of the accent bars (the default indigo #5147e5, the only accent in the rest face): per sample
  # (top + bottom) / 2, then the median of 15 samples 70 ms apart.
  bars_centre() {
    local name=$1
    for i in $(seq 15); do
      grim -g "0,0 853x48" -t ppm "$OUT/$name-$i.ppm"
      sleep 0.07 # real time: the waveform loop runs on the compositor clock
    done
    python3 -I -c "
import statistics, sys
centres = []
for path in sys.argv[1:]:
    d = open(path, \"rb\").read()
    _, w, h, _, px = d.split(maxsplit=4)
    w, h = int(w), int(h)
    rows = [y for y in range(h) for x in range(w)
            if px[3*(y*w+x)+2] > 150 and px[3*(y*w+x)+2] > px[3*(y*w+x)] + 60 and px[3*(y*w+x)+2] > px[3*(y*w+x)+1] + 60]
    if rows:
        centres.append((min(rows) + max(rows)) / 2)
print(statistics.median(centres) if centres else -1)
" "$OUT/$name-"*.ppm | tee "$OUT/bars-$name.txt"
  }

  python3 "$(dirname "$NOCTALIA")/../../tests/e2e/mpris_test_player.py" "$OUT/player-calls.txt" &
  for _ in $(seq 50); do gdbus call --session -d org.freedesktop.DBus -o / -m org.freedesktop.DBus.NameHasOwner org.mpris.MediaPlayer2.e2e 2>/dev/null | grep -q true && break; sleep 0.1; done
  sleep 3 # real time: the island grows into the media face and the position probe gives up
  before=$(bars_centre before)
  [[ $before != -1 ]] || fail "no waveform bars found in the island"

  "$ANDROMEDA" msg workspace-next > /dev/null
  sleep 1.5 # real time: the switch animation settles
  after_next=$(bars_centre after-next)
  "$ANDROMEDA" msg workspace-previous > /dev/null
  sleep 1.5 # real time: the switch animation settles
  after_prev=$(bars_centre after-previous)

  # Hovering the island grows it into the media hover face; leaving collapses it back to rest.
  POINTER=$(dirname "$ANDROMEDA")/tests/pointer-client
  "$POINTER" 853 480 move 426 16 > /dev/null
  sleep 1.5 # real time: the hover face opens
  "$POINTER" 853 480 move 426 400 > /dev/null
  sleep 1.5 # real time: the island collapses back to rest
  after_hover=$(bars_centre after-hover)

  echo "bars centre row: before $before, after next $after_next, after previous $after_prev, after hover $after_hover"
  near() { awk -v a="$1" -v b="$2" "BEGIN { d = a - b; exit !(d <= 1 && d >= -1) }"; }
  near "$after_next" "$before" || fail "bars moved after workspace-next: centre row $before -> $after_next"
  near "$after_prev" "$before" || fail "bars moved after workspace-previous: centre row $before -> $after_prev"
  near "$after_hover" "$before" || fail "bars moved after hovering the island: centre row $before -> $after_hover"
  echo "PASS"
'
