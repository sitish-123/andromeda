#!/usr/bin/env bash
# Screen recording encodes on the GPU and saves an MP4: with NOCTALIA_RECORD_TEST_SOURCE standing in for the
# ScreenCast portal stream, `noctalia msg screen-record-toggle` starts vapostproc ! vah264enc ! mp4mux, a second toggle
# stops it, and the file in ~/Videos must be H.264 of about the recorded length. Writes probe.txt and the recording
# to $OUT (default ./artifacts/screen-record).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/screen-record}
source "$(dirname "$0")/lib.sh"
boot_headless 1
export NOCTALIA_RECORD_TEST_SOURCE=1

with_noctalia '
  [[ $("$NOCTALIA" msg screen-record-toggle) == ok ]] || { echo "FAIL: recording did not start"; exit 1; }
  [[ $("$NOCTALIA" msg screen-record-status) == on ]] || { echo "FAIL: status not on"; exit 1; }
  sleep 3 # real time: the recorded length
  [[ $("$NOCTALIA" msg screen-record-toggle) == ok ]] || { echo "FAIL: recording did not stop"; exit 1; }
  for _ in $(seq 50); do [[ $("$NOCTALIA" msg screen-record-status) == off ]] && break; sleep 0.1; done
  grep -q "recording saved to" "$OUT/noctalia.log" || { echo "FAIL: not saved"; grep -i record "$OUT/noctalia.log"; exit 1; }
  cp "$(sed -n "s/.*recording saved to //p" "$OUT/noctalia.log" | tail -1)" "$OUT/recording.mp4"
'
ffprobe -v error -select_streams v:0 -count_frames \
  -show_entries stream=codec_name,width,height,nb_read_frames:format=duration -of default=nw=1 \
  "$OUT/recording.mp4" | tee "$OUT/probe.txt"
grep -qx "codec_name=h264" "$OUT/probe.txt" || { echo "FAIL: not H.264"; exit 1; }
duration=$(sed -n "s/^duration=//p" "$OUT/probe.txt")
awk -v d="$duration" 'BEGIN {exit !(d >= 2.0 && d <= 5.0)}' || { echo "FAIL: duration $duration s"; exit 1; }
echo "PASS; artifacts: $OUT"
