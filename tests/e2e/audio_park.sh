#!/usr/bin/env bash
# andromeda-audio-park with a fake Bluetooth sink (a null sink named bluez_output.e2e): a stream of zeros on it moves
# to the park sink after about 5 s and the fake sink suspends; sound in the stream moves it back. Leaves no module.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
FAKE=bluez_output.e2e
PARK=e2e_park
DIR=$(mktemp -d /tmp/audio-park.XXXX)
fail() { echo "FAIL: $*" >&2; exit 1; }
sink_of() { pactl list short sink-inputs | awk -F'\t' -v i="$1" '$1 == i { print $2 }'; }
sink_index() { pactl list short sinks | awk -F'\t' -v n="$1" '$2 == n { print $1 }'; }
state_of() { pactl list short sinks | awk -F'\t' -v n="$1" '$2 == n { print $5 }'; }
# wait_for SECONDS COMMAND...: poll COMMAND every 0.2 s.
wait_for() { local t=$1; shift; for _ in $(seq $((t * 5))); do "$@" && return 0; sleep 0.2; done; return 1; }

systemctl --user is-active -q andromeda-audio-park && fail "andromeda-audio-park runs and would act on the fake sink"
mod=$(pactl load-module module-null-sink "sink_name=$FAKE")
cleanup() {
  kill $(jobs -p) 2> /dev/null || true
  wait 2> /dev/null || true
  pactl unload-module "$mod" 2> /dev/null || true
  rm -rf "$DIR"
}
trap cleanup EXIT

# The stream plays zeros until $DIR/loud exists, then noise.
(while :; do if [[ -e $DIR/loud ]]; then head -c 9600 /dev/urandom; else head -c 9600 /dev/zero; fi; done) |
  pacat --raw --format=s16le --channels=2 --rate=48000 --latency-msec=50 -d "$FAKE" --client-name=e2e-zen &
fake=$(sink_index "$FAKE")
wait_for 5 bash -c "pactl list short sink-inputs | awk -F'\t' '\$2 == $fake' | grep -q ." || fail "no stream on $FAKE"
stream=$(pactl list short sink-inputs | awk -F'\t' -v s="$fake" '$2 == s { print $1 }')
echo "stream $stream on $FAKE ($fake)"

ANDROMEDA_PARK_MATCH=$FAKE ANDROMEDA_PARK_SINK=$PARK "$ROOT/session/audio-park/andromeda-audio-park" 2> "$DIR/park.log" &
pid=$!
start=$EPOCHREALTIME
wait_for 15 bash -c "[[ \$(pactl list short sinks | awk -F'\t' '\$2 == \"$PARK\" { print \$1 }') == \$(pactl list short sink-inputs | awk -F'\t' '\$1 == $stream { print \$2 }') ]]" ||
  fail "stream not parked: $(cat "$DIR/park.log")"
echo "parked after $(awk "BEGIN { printf \"%.1f\", $EPOCHREALTIME - $start }") s"
wait_for 15 bash -c "[[ \$(pactl list short sinks | awk -F'\t' '\$2 == \"$FAKE\" { print \$5 }') == SUSPENDED ]]" ||
  fail "$FAKE did not suspend: $(state_of "$FAKE")"
echo "$FAKE suspended"

touch "$DIR/loud"
start=$EPOCHREALTIME
wait_for 10 bash -c "[[ \$(pactl list short sink-inputs | awk -F'\t' '\$1 == $stream { print \$2 }') == $fake ]]" ||
  fail "stream not back on $FAKE: on $(sink_of "$stream"); $(cat "$DIR/park.log")"
echo "back after $(awk "BEGIN { printf \"%.1f\", $EPOCHREALTIME - $start }") s"
wait_for 5 bash -c "! pactl list short modules | grep -q 'sink_name=$PARK'" || fail "park module left after unpark"

rm "$DIR/loud"
wait_for 15 bash -c "[[ \$(pactl list short sinks | awk -F'\t' '\$2 == \"$PARK\" { print \$1 }') == \$(pactl list short sink-inputs | awk -F'\t' '\$1 == $stream { print \$2 }') ]]" ||
  fail "stream not parked again"
kill "$pid"; wait "$pid" || true
[[ $(sink_of "$stream") == "$fake" ]] || fail "stop left the stream on $(sink_of "$stream")"
pactl list short modules | grep -q "sink_name=$PARK" && fail "stop left the park module"
cat "$DIR/park.log"
echo PASS
