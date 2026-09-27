#!/usr/bin/env bash
# Screen recording encodes on the GPU and saves an MP4 with the desktop's audio. NOCTALIA_RECORD_TEST_SOURCE stands in
# for the ScreenCast portal stream. With no PipeWire in the sandbox, `noctalia msg screen-record-toggle` still records
# H.264 video only. Then a private PipeWire and WirePlumber start with a null sink as the default output playing a
# 440 Hz tone, and the second recording must carry H.264 video plus AAC audio of about the recorded length whose loudness
# shows the tone was captured from the sink's monitor. Writes probe-video-only.txt, probe.txt, volume.txt and both
# recordings to $OUT (default ./artifacts/screen-record).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/screen-record}
source "$(dirname "$0")/lib.sh"
boot_headless 1
export NOCTALIA_RECORD_TEST_SOURCE=1
rm -f "$OUT"/*.mp4 "$OUT"/*.txt

with_noctalia '
  fail() { echo "FAIL: $*" >&2; exit 1; }
  # record NAME: a 3 s recording copied to $OUT/NAME.mp4.
  record() {
    local saved
    [[ $("$NOCTALIA" msg screen-record-toggle) == ok ]] || fail "recording did not start"
    [[ $("$NOCTALIA" msg screen-record-status) == on ]] || fail "status not on"
    sleep 3 # real time: the recorded length
    saved=$(grep -c "recording saved to" "$OUT/noctalia.log" || true)
    [[ $("$NOCTALIA" msg screen-record-toggle) == ok ]] || fail "recording did not stop"
    for _ in $(seq 50); do [[ $(grep -c "recording saved to" "$OUT/noctalia.log") -gt $saved ]] && break; sleep 0.1; done
    [[ $(grep -c "recording saved to" "$OUT/noctalia.log") -gt $saved ]] || fail "not saved: $(grep -i record "$OUT/noctalia.log")"
    cp "$(sed -n "s/.*recording saved to //p" "$OUT/noctalia.log" | tail -1)" "$OUT/$1.mp4"
  }

  record video-only
  grep -q "recording without audio" "$OUT/noctalia.log" || fail "no fallback to video only without PipeWire"

  mkdir -p "$RUNTIME/pw-config"
  XDG_CONFIG_HOME=$RUNTIME/pw-config pipewire > "$OUT/pipewire.log" 2>&1 &
  for _ in $(seq 50); do [[ -S $RUNTIME/pipewire-0 ]] && break; sleep 0.1; done
  XDG_CONFIG_HOME=$RUNTIME/pw-config wireplumber > "$OUT/wireplumber.log" 2>&1 &
  sleep 1 # real time: WirePlumber connects and loads its policy
  pw-cli create-node adapter "{ factory.name=support.null-audio-sink node.name=e2e-sink media.class=Audio/Sink object.linger=true audio.position=[FL FR] }" > /dev/null
  for _ in $(seq 50); do wpctl status 2>/dev/null | grep -q e2e-sink && break; sleep 0.1; done
  sink=$(pw-cli ls Node | awk "/^\tid / {id = \$2} /node.name = \"e2e-sink\"/ {print id}" | tr -d ,)
  wpctl set-default "$sink"
  ffmpeg -v error -f lavfi -i "sine=frequency=440:duration=30" -ac 2 "$RUNTIME/tone.wav"
  pw-play --target e2e-sink "$RUNTIME/tone.wav" &
  sleep 0.5 # real time: the tone reaches the sink

  record recording
  grep -q "with desktop audio" "$OUT/noctalia.log" || fail "second recording has no audio branch"
'
ffprobe -v error -show_entries stream=codec_name -of default=nw=1 "$OUT/video-only.mp4" | tee "$OUT/probe-video-only.txt"
[[ $(cat "$OUT/probe-video-only.txt") == codec_name=h264 ]] || { echo "FAIL: video-only recording is not H.264 alone"; exit 1; }
ffprobe -v error -show_entries stream=codec_name,width,height:format=duration -of default=nw=1 \
  "$OUT/recording.mp4" | tee "$OUT/probe.txt"
grep -qx "codec_name=h264" "$OUT/probe.txt" || { echo "FAIL: not H.264"; exit 1; }
grep -qx "codec_name=aac" "$OUT/probe.txt" || { echo "FAIL: no AAC audio"; exit 1; }
duration=$(sed -n "s/^duration=//p" "$OUT/probe.txt")
awk -v d="$duration" 'BEGIN {exit !(d >= 2.0 && d <= 5.0)}' || { echo "FAIL: duration $duration s"; exit 1; }
ffmpeg -hide_banner -i "$OUT/recording.mp4" -map 0:a -af volumedetect -f null - 2>&1 | grep -E "mean_volume|max_volume" \
  | tee "$OUT/volume.txt"
mean=$(sed -n "s/.*mean_volume: \(-\?[0-9.]*\) dB/\1/p" "$OUT/volume.txt")
awk -v m="$mean" 'BEGIN {exit !(m > -30)}' || { echo "FAIL: audio is silent (mean $mean dB)"; exit 1; }
echo "PASS; artifacts: $OUT"
