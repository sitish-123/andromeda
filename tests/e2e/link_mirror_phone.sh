#!/usr/bin/env bash
# Mirroring a real phone's screen and sound to a private umbriel-linkd over Wi-Fi, through a relay that can impair
# the phone-to-desktop direction (tests/e2e/udp_impair.py), so loss is real without root.
#
# Proves:
# 1. Latency: the fixture flips the whole screen black or white at each touch; the desktop taps it through
#    MirrorInput 30 times and times each flip's arrival in the viewer's decoded frames (umbriel-link-mirror --luma).
#    The input path, capture, encode, network, and decode are all in the measure. Median under 250 ms.
# 2. Bitrate under loss: while the fixture draws noise, 3 Mbit/s and 2 % loss on the relay make the phone step its
#    encoder down from 8 Mbit/s; after the link is clean again it steps back up. Frames keep arriving throughout.
# 3. Sound: the fixture's 440 Hz tone arrives as Opus and decodes to audio.wav with that pitch.
# Needs the debug APKs and the phone on USB debugging, on the same Wi-Fi as this machine; see link_phone_lib.sh.
# Updates Link on the phone, keeps its other pairings, and unpairs the run's desktop after. Holds
# /tmp/umbriel-link-phone.lock, which other phone runs take too. Writes results.jsonl, latency.tsv, luma.tsv, relay.tsv,
# bitrate.log, audio.wav, linkd.log, viewer.log, logcat.txt to $OUT (default ./artifacts/link-mirror-phone).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/link-mirror-phone}
source "$(dirname "$0")/link_phone_lib.sh"
# A UDP port this machine's firewall lets in; the daemon's own random port is not, so the phone can only use the relay.
RELAY_PORT=${RELAY_PORT:-48307}
TAPS=30

# The lock stays with flock itself, so a helper left running cannot keep it.
[[ -n ${LINK_PHONE_LOCKED:-} ]] || exec env LINK_PHONE_LOCKED=1 flock --close /tmp/umbriel-link-phone.lock "$0" "$@"
rm -rf "$OUT"
mkdir -p "$OUT"
RUNTIME=$(mktemp -d /tmp/link-mirror-phone.XXXX)
trap 'forget_phone; kill $(jobs -p) 2>/dev/null || true; wait 2>/dev/null; rm -rf "$RUNTIME"' EXIT
[[ $(adb get-state 2>/dev/null) == device ]] || fail "no phone at $SERIAL"
[[ -s $APK && -s $FIXTURE ]] || fail "no APKs at $APK and $FIXTURE"

private_bus system
start_linkd
HOST=$(ip -4 route get 1.1.1.1 | grep -o 'src [0-9.]*' | cut -d' ' -f2)
echo clean > "$RUNTIME/impair"
python3 "$ROOT/tests/e2e/udp_impair.py" "$RELAY_PORT" "$PORT" "$RUNTIME/impair" "$OUT/relay.tsv" &

install_app
# The phone dials the relay, which the desktop sees as 127.0.0.1.
pair_phone "$HOST:$RELAY_PORT"
grep -q "connected over IP from .*127.0.0.1" "$OUT/linkd.log" || fail "the phone did not come through the relay"
record "{\"step\":\"paired-through-relay\",\"device\":\"$ID\",\"relay\":\"$HOST:$RELAY_PORT\"}"
phone_switches screen
# After the app's restart: a force-stop can leave the service unbound.
adb shell settings put secure enabled_accessibility_services "$PACKAGE/$PACKAGE.screen.ScreenInput"
adb shell settings put secure accessibility_enabled 1
wait_for 15 "Android did not bind ScreenInput" eval 'adb shell dumpsys accessibility | grep -q "Bound services:{Service\[label=Link"'
grep "connected over IP" "$OUT/linkd.log" | tail -1 | grep -q 127.0.0.1 || fail "the phone reconnected around the relay"

# start_mirror ARGS...: a viewer with ARGS, through the request notification and Android's capture prompt.
start_mirror() {
  "$BIN/umbriel-link-mirror" "$ID" "$@" >> "$OUT/viewer.log" 2>&1 &
  VIEWER=$!
  allow_mirror "${1#--}"
  wait_for 20 "the mirror did not start" eval 'grep -q "mirroring at" "$OUT/linkd.log"'
}

adb logcat -c
adb logcat -v epoch ScreenCapture:I ScreenInput:I MirrorAudio:I LinkFixture:I '*:S' > "$OUT/logcat.txt" &
adb shell am start -W -n org.umbriel.link.fixture/.MirrorProbeActivity > /dev/null
sleep 1
start_mirror --luma "$OUT/luma.tsv" --count 1000000
adb shell am start -W -n org.umbriel.link.fixture/.MirrorProbeActivity > /dev/null
wait_for 15 "no frames decoded" test -s "$OUT/luma.tsv"
sleep 3

: > "$OUT/taps.tsv"
for tap in $(seq "$TAPS"); do
  printf '%s %s\n' "$tap" "$(now_ms)" >> "$OUT/taps.tsv"
  link MirrorInput "'$ID'" tap "{'x': <int32 5000>, 'y': <int32 5000>}" > /dev/null
  sleep "0.$(( 70 + RANDOM % 30 ))"
done
sleep 1
grep -c flip "$OUT/logcat.txt" > /dev/null || fail "the fixture saw no touches"
python3 - "$OUT/taps.tsv" "$OUT/luma.tsv" "$OUT/latency.tsv" > "$RUNTIME/latency.json" <<'PY'
import json, statistics, sys
taps = [tuple(map(int, line.split())) for line in open(sys.argv[1])]
frames = [tuple(map(int, line.split())) for line in open(sys.argv[2])]
lo, hi = min(l for _, l in frames), max(l for _, l in frames)
mid = (lo + hi) / 2
bright = lambda luma: luma > mid
# The screen starts black; tap k leaves it white when k is odd.
latencies, out = [], open(sys.argv[3], "w")
for k, sent in taps:
    want = k % 2 == 1
    arrived = next((t for t, l in frames if t > sent and bright(l) == want), None)
    if arrived is None:
        continue
    latencies.append(arrived - sent)
    print(k, sent, arrived, arrived - sent, file=out)
latencies.sort()
q = lambda p: latencies[min(len(latencies) - 1, int(p * len(latencies)))]
print(json.dumps({"taps": len(taps), "measured": len(latencies), "luma": [lo, hi],
                  "p50_ms": q(0.5), "p90_ms": q(0.9), "min_ms": latencies[0], "max_ms": latencies[-1],
                  "mean_ms": round(statistics.mean(latencies))}))
PY
LATENCY=$(cat "$RUNTIME/latency.json")
record "{\"step\":\"tap-to-photon\",\"latency\":$LATENCY}"
P50=$(jq .p50_ms <<< "$LATENCY")
MEASURED=$(jq .measured <<< "$LATENCY")
(( MEASURED >= TAPS * 9 / 10 )) || fail "only $MEASURED of $TAPS flips arrived"
(( P50 < 250 )) || fail "median tap-to-photon $P50 ms"

frames_since() { awk -v since="$1" '$1 > since' "$OUT/luma.tsv" | wc -l; }
bitrates() { grep -o 'bitrate [0-9]* kbit/s' "$OUT/logcat.txt" | grep -o '[0-9]*' || true; }
# Random blocks every frame keep the encoder at its full rate, so the loss bites.
adb shell am start -n org.umbriel.link.fixture/.MirrorProbeActivity --ez noise true > /dev/null
sleep 3
START=$(now_ms)
STEPS=$(bitrates | wc -l)
echo "rate=3000000 loss=0.02" > "$RUNTIME/impair"
sleep 15
IMPAIRED=$(frames_since "$START")
DOWN=$(bitrates | tail -n +$(( STEPS + 1 )) | sort -n | head -1)
[[ -n $DOWN ]] && (( DOWN < 8000 )) || fail "no step down under loss: $(bitrates | tr '\n' ' ')"
echo clean > "$RUNTIME/impair"
CLEAN=$(now_ms)
sleep 30
RECOVERED=$(frames_since "$CLEAN")
UP=$(bitrates | tail -1)
(( UP > DOWN )) || fail "no step up after the loss ended: $(bitrates | tr '\n' ' ')"
(( IMPAIRED > 15 * 5 && RECOVERED > 30 * 20 )) || fail "frames stalled: $IMPAIRED under loss, $RECOVERED after"
grep 'bitrate\|far behind' "$OUT/logcat.txt" > "$OUT/bitrate.log" || true
record "{\"step\":\"bitrate-under-loss\",\"impairment\":\"3 Mbit/s, 2% loss, 15 s\",\"lowest_kbit\":$DOWN,\"after_30s_clean_kbit\":$UP,\"frames_impaired\":$IMPAIRED,\"frames_after\":$RECOVERED,\"steps\":\"$(bitrates | tr '\n' ' ')\"}"
kill "$VIEWER"
wait "$VIEWER" 2> /dev/null || true
wait_for 10 "the capture kept running" eval '! adb shell dumpsys activity services "$PACKAGE" | grep -q ScreenCapture'

# A still screen again: uiautomator cannot read the screen while the noise animates.
adb shell am force-stop org.umbriel.link.fixture
adb shell am start -W -n org.umbriel.link.fixture/.MirrorProbeActivity > /dev/null
"$BIN/umbriel-link-mirror" "$ID" --frames "$OUT/frames" --count 300 >> "$OUT/viewer.log" 2>&1 &
VIEWER=$!
allow_mirror sound
# Noise again, now that the prompt is done: a still screen sends too few frames to finish the viewer's count.
adb shell am start -W -n org.umbriel.link.fixture/.MirrorProbeActivity --ez noise true > /dev/null
wait "$VIEWER" || fail "the viewer failed: $(tail -3 "$OUT/viewer.log")"
[[ -s $OUT/frames/frame-010.png ]] || fail "no frames in the sound run"
[[ -s $OUT/frames/audio.wav ]] || fail "no sound arrived: $(grep -i 'MirrorAudio\|sound' "$OUT/logcat.txt" "$OUT/viewer.log" | tail -3)"
mv "$OUT/frames/audio.wav" "$OUT/audio.wav"
python3 - "$OUT/audio.wav" > "$RUNTIME/sound.json" <<'PY'
import json, math, struct, sys, wave
w = wave.open(sys.argv[1])
rate, channels, frames = w.getframerate(), w.getnchannels(), w.getnframes()
data = struct.unpack(f"<{frames * channels}h", w.readframes(frames))
left = data[::channels]
# The last second, past any start-up silence.
tail = left[-rate:]
rms = math.sqrt(sum(s * s for s in tail) / max(len(tail), 1))
crossings = sum(1 for a, b in zip(tail, tail[1:]) if a < 0 <= b)
print(json.dumps({"seconds": round(frames / rate, 2), "rate": rate, "channels": channels,
                  "rms": round(rms), "pitch_hz": crossings * rate // max(len(tail), 1)}))
PY
SOUND=$(cat "$RUNTIME/sound.json")
PITCH=$(jq .pitch_hz <<< "$SOUND")
RMS=$(jq .rms <<< "$SOUND")
(( PITCH > 420 && PITCH < 460 && RMS > 1000 )) || fail "the sound is not the fixture's tone: $SOUND"
record "{\"step\":\"sound\",\"audio\":$SOUND}"
adb shell am force-stop org.umbriel.link.fixture
cat "$OUT/results.jsonl"
echo "PASS; artifacts: $OUT"
