#!/usr/bin/env bash
# Region recording. With NOCTALIA_RECORD_TEST_SOURCE (a 1280x720 SMPTE pattern standing in for the portal stream at
# 0,0), `screen-record-region "100,50 600x400"` saves a 600x400 H.264 MP4 whose first frame matches the pattern cropped
# at 100,50 and not the whole pattern scaled down; a region off the recorded display is refused; with no geometry the
# region picker appears over the desktop, and Escape cancels it without recording. Writes region.mp4, frame.png,
# reference.png, desktop.png, picker.png, picker.txt, and compare.txt to $OUT (default ./artifacts/screen-record-region).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/screen-record-region}
source "$(dirname "$0")/lib.sh"
boot_headless 1
export NOCTALIA_RECORD_TEST_SOURCE=1
rm -f "$OUT"/*.mp4 "$OUT"/*.png "$OUT"/*.txt

with_noctalia '
  fail() { echo "FAIL: $*" >&2; exit 1; }
  msg() { "$NOCTALIA" msg "$@" 2>&1 || true; }

  [[ $(msg screen-record-region "100,50 600x400") == ok ]] || fail "region recording did not start"
  sleep 3 # real time: the recorded length
  [[ $(msg screen-record-region) == ok ]] || fail "region recording did not stop"
  for _ in $(seq 50); do grep -q "recording saved to" "$OUT/noctalia.log" && break; sleep 0.1; done
  cp "$(sed -n "s/.*recording saved to //p" "$OUT/noctalia.log" | tail -1)" "$OUT/region.mp4"

  [[ $(msg screen-record-region "5000,5000 100x100") == error* ]] || fail "an off-display region was accepted"
  grep -q "the region is not on the recorded display" "$OUT/noctalia.log" || fail "no reason for the refused region"
  [[ $(msg screen-record-status) == off ]] || fail "a refused region left a recording running"

  grim "$OUT/desktop.png"
  [[ $(msg screen-record-region) == ok ]] || fail "the region picker did not open"
  sleep 1 # real time: the overlay maps and paints
  grim "$OUT/picker.png"
  mean() { magick "$1" -colorspace Gray -format "%[fx:mean]" info:; }
  echo "mean_desktop=$(mean "$OUT/desktop.png") mean_picker=$(mean "$OUT/picker.png")" > "$OUT/picker.txt"
  awk -v d="$(mean "$OUT/desktop.png")" -v p="$(mean "$OUT/picker.png")" "BEGIN {exit !(p < d * 0.8)}" \
    || fail "no dimmed picker over the desktop: $(cat "$OUT/picker.txt")"
  wtype -k Escape
  sleep 1
  [[ $(msg screen-record-status) == off ]] || fail "cancelling the picker started a recording"
'
ffprobe -v error -show_entries stream=codec_name,width,height -of default=nw=1 "$OUT/region.mp4" | tee "$OUT/probe.txt"
grep -qx "width=600" "$OUT/probe.txt" && grep -qx "height=400" "$OUT/probe.txt" || { echo "FAIL: region is not 600x400"; exit 1; }
ffmpeg -v error -y -i "$OUT/region.mp4" -frames:v 1 "$OUT/frame.png"
gst-launch-1.0 -q videotestsrc num-buffers=1 pattern=smpte ! video/x-raw,width=1280,height=720 ! videoconvert ! pngenc \
  ! filesink location="$OUT/full.png"
magick "$OUT/full.png" -crop 600x400+100+50 +repage "$OUT/reference.png"
magick "$OUT/full.png" -resize '600x400!' "$OUT/scaled.png"
# compare exits 1 whenever the images differ at all.
rmse() { { magick compare -metric RMSE "$1" "$2" null: 2>&1 || true; } | sed -n "s/.*(\(.*\))/\1/p"; }
cropped=$(rmse "$OUT/frame.png" "$OUT/reference.png")
scaled=$(rmse "$OUT/frame.png" "$OUT/scaled.png")
echo "rmse_vs_crop=$cropped rmse_vs_scaled=$scaled" | tee "$OUT/compare.txt"
rm "$OUT/full.png" "$OUT/scaled.png"
awk -v c="$cropped" -v s="$scaled" 'BEGIN {exit !(c < 0.1 && c < s)}' || { echo "FAIL: the frame is not the cropped region"; exit 1; }
echo "PASS; artifacts: $OUT"
