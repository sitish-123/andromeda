#!/usr/bin/env bash
# Link sessions and shares across two network namespaces joined by a veth pair, inside an unprivileged user namespace.
# Proves: a present phone (keep-alive) shows connected on D-Bus and stays on the same connection through a 5 s black
# hole; a 35 s black hole ends the session on both sides, D-Bus Share then fails as not connected, and the phone
# redials by itself once the path returns; it also redials after a daemon restart; text and links travel both ways
# (D-Bus Share to the phone's event output, the phone's shares to the D-Bus Received signal); an oversized text and
# non-http links are refused by D-Bus and by the phone before sending, and a hostile phone that skips its checks is
# closed with protocol-error and delivers nothing; stopping the phone shows it disconnected at once. Every control
# message is validated against protocol/link-v1/messages.cddl, and the hostile phone's transcript must fail it.
# Writes results.jsonl, hold.jsonl, signals.txt, transcript.jsonl, hostile.jsonl, linkd.log to $OUT
# (default ./artifacts/link-share).
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
OUT=${OUT:-$(pwd)/artifacts/link-share}
BIN=${LINK_BIN:-$ROOT/link/target/debug}
if [[ -z ${LINK_IN_NS:-} ]]; then
  rm -rf "$OUT"
  mkdir -p "$OUT"
  exec env LINK_IN_NS=1 OUT="$OUT" BIN="$BIN" unshare --user --map-root-user --net bash "$0"
fi

RUNTIME=$(mktemp -d /tmp/link-share.XXXX)
trap 'kill $(jobs -p) 2>/dev/null || true; wait 2>/dev/null; rm -rf "$RUNTIME"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }
record() { printf '%s\n' "$1" >> "$OUT/results.jsonl"; }
# wait_for SECONDS DESCRIPTION CMD...: poll CMD every 0.1 s.
wait_for() {
  local tries=$(( $1 * 10 )) what=$2; shift 2
  for _ in $(seq "$tries"); do "$@" && return; sleep 0.1; done
  fail "$what"
}

ip link set lo up
ip link add d0 type veth peer name p0
unshare --net sleep infinity &
PHONE_NS=$!
for _ in $(seq 50); do [[ $(readlink /proc/$PHONE_NS/ns/net) != "$(readlink /proc/self/ns/net)" ]] && break; sleep 0.02; done
ip link set p0 netns "$PHONE_NS"
ip addr add 10.78.0.1/24 dev d0
ip link set d0 up
in_phone() { nsenter -t "$PHONE_NS" -n -- "$@"; }
in_phone ip link set lo up
in_phone ip addr add 10.78.0.2/24 dev p0
in_phone ip link set p0 up

cat > "$RUNTIME/bus.conf" <<CONF
<busconfig>
  <type>session</type>
  <listen>unix:path=$RUNTIME/bus</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow user="*"/>
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
CONF
dbus-daemon --config-file="$RUNTIME/bus.conf" --nofork 2> "$OUT/dbus.log" &
for _ in $(seq 50); do [[ -S $RUNTIME/bus ]] && break; sleep 0.02; done
export DBUS_SESSION_BUS_ADDRESS=unix:path=$RUNTIME/bus DBUS_SYSTEM_BUS_ADDRESS=unix:path=$RUNTIME/bus

link() { gdbus call --session -d org.umbriel.Link1 -o /org/umbriel/Link1 -m "org.umbriel.Link1.$1" "${@:2}"; }
devices() { gdbus call --session -d org.umbriel.Link1 -o /org/umbriel/Link1 -m org.freedesktop.DBus.Properties.Get org.umbriel.Link1 Devices; }
connected() { devices | grep -q "'phone', true"; }
disconnected() { devices | grep -q "'phone', false"; }
start_linkd() {
  XDG_STATE_HOME=$RUNTIME/desktop "$BIN/umbriel-linkd" >> "$OUT/linkd.log" 2>&1 &
  LINKD=$!
  for _ in $(seq 100); do link CancelPairing > /dev/null 2>&1 && return; sleep 0.05; done
  fail "umbriel-linkd did not come up"
}
port() { python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["port"])' "$RUNTIME/desktop/umbriel-link/devices.json"; }
phone() { in_phone "$BIN/umbriel-link-phone" --state "$RUNTIME/phone" --name phone "$@"; }
events() { grep -c "\"event\":\"$1\"" "$OUT/hold.jsonl" || true; }
signal_count() { grep -cF "string \"$1\"" "$OUT/signals.txt" || true; }
exited() { ! kill -0 "$1" 2> /dev/null || grep -q '^State:.*Z' "/proc/$1/status"; }
say() { printf '%s\n' "$*" >&3; }
blackhole() { in_phone tc qdisc add dev p0 root netem loss 100%; }
clear_path() { in_phone tc qdisc del dev p0 root; }

start_linkd
PORT=$(port)
[[ $PORT == 4717 ]] || fail "a new store did not get the default port: $PORT"
record "{\"step\":\"default-port\",\"port\":$PORT}"

CODE=$(link StartPairing | sed -E "s/^\('([0-9]+)', .*/\1/")
phone --transcript "$OUT/transcript.jsonl" pair --code "$CODE" --addr "10.78.0.1:$PORT" > /dev/null || fail "pairing"
ID=$(devices | grep -o "'[0-9a-f]\{32\}', 'phone'" | cut -d"'" -f2)
[[ -n $ID ]] || fail "desktop does not list the phone: $(devices)"

OUTPUT=$(link Share "$ID" text "'nobody is there'" 2>&1) && fail "Share to a disconnected phone succeeded"
[[ $OUTPUT == *org.umbriel.Link1.Error.NotConnected* ]] || fail "Share while disconnected: $OUTPUT"
record '{"step":"share-needs-a-session","error":"NotConnected"}'

dbus-monitor --session "type='signal',interface='org.umbriel.Link1',member='Received'" > "$OUT/signals.txt" 2>&1 &
wait_for 5 "dbus-monitor did not start" test -s "$OUT/signals.txt"

mkfifo "$RUNTIME/phone.in"
exec 3<> "$RUNTIME/phone.in"
# Not through phone(): $! must be the phone itself (nsenter execs it), so that kill reaches it.
RUST_LOG=info nsenter -t "$PHONE_NS" -n -- "$BIN/umbriel-link-phone" --state "$RUNTIME/phone" --name phone \
  --transcript "$OUT/transcript.jsonl" hold < "$RUNTIME/phone.in" > "$OUT/hold.jsonl" 2> "$OUT/hold.log" &
HOLD=$!
wait_for 10 "present phone never shown connected: $(devices)" connected
record '{"step":"present-connected"}'

link Share "$ID" text "'héllo from the desktop ✓'" > /dev/null || fail "D-Bus Share text"
link Share "$ID" link "'https://example.org/path?q=1'" > /dev/null || fail "D-Bus Share link"
grep -q '"event":"received".*"kind":"text","text":"héllo from the desktop ✓"' "$OUT/hold.jsonl" || fail "phone missed the text"
grep -q '"kind":"link","text":"https://example.org/path?q=1"' "$OUT/hold.jsonl" || fail "phone missed the link"
record '{"step":"desktop-to-phone","kinds":["text","link"]}'

say "text hello from the phone"
say "link https://example.com/from-phone"
wait_for 10 "no Received signal for the phone's text" eval '[[ $(signal_count "hello from the phone") == 1 ]]'
wait_for 10 "no Received signal for the phone's link" eval '[[ $(signal_count "https://example.com/from-phone") == 1 ]]'
grep -A3 "member=Received" "$OUT/signals.txt" | grep -qF "string \"$ID\"" || fail "Received does not name the phone"
wait_for 5 "phone did not see both acks" eval '[[ $(events shared) == 2 ]]'
record '{"step":"phone-to-desktop","kinds":["text","link"]}'

OUTPUT=$(link Share "$ID" text "'$(head -c 61441 /dev/zero | tr '\0' a)'" 2>&1) && fail "an oversized text was sent"
[[ $OUTPUT == *Error.Rejected*longer\ than\ 61440* ]] || fail "oversized Share: $OUTPUT"
for bad in "ftp://example.org/" "javascript:alert(1)" "https://example.org/a b"; do
  OUTPUT=$(link Share "$ID" link "'$bad'" 2>&1) && fail "link $bad was sent"
  [[ $OUTPUT == *Error.Rejected*http\ or\ https* ]] || fail "Share link $bad: $OUTPUT"
done
say "link file:///etc/passwd"
wait_for 5 "phone sent a file link" eval '[[ $(events share-failed) == 1 ]]'
grep '"event":"share-failed"' "$OUT/hold.jsonl" | grep -q 'http or https' || fail "phone refused the file link for another reason"
record '{"step":"refused-before-sending","dbus":["oversize","ftp","javascript","space"],"phone":["file"]}'

blackhole
sleep 5
clear_path
link Share "$ID" text "'after a short drop'" > /dev/null || fail "session did not survive a 5 s drop"
[[ $(events connected) == 1 && $(events disconnected) == 0 ]] || fail "a 5 s drop replaced the connection"
record '{"step":"survives-short-drop","seconds":5}'

blackhole
wait_for 40 "desktop still shows the phone connected in a black hole" disconnected
wait_for 10 "phone never noticed the black hole" eval '[[ $(events disconnected) == 1 ]]'
OUTPUT=$(link Share "$ID" text "'into the void'" 2>&1) && fail "Share succeeded during the black hole"
[[ $OUTPUT == *NotConnected* ]] || fail "Share in the black hole: $OUTPUT"
clear_path
wait_for 40 "phone did not redial after the black hole" eval '[[ $(events connected) == 2 ]]'
wait_for 5 "desktop does not show the redialled phone" connected
record '{"step":"redials-after-long-drop","black_hole_seconds":"30+"}'

kill "$LINKD"; wait "$LINKD" || true
start_linkd
wait_for 40 "phone did not redial after a daemon restart" eval '[[ $(events connected) == 3 ]]'
wait_for 5 "desktop does not show the phone after its restart" connected
grep '"event":"connected"' "$OUT/hold.jsonl" | tail -1 | grep -q '"via":"last-known"' || fail "redial skipped the last-known address"
link Share "$ID" link "'https://example.org/after-restart'" > /dev/null || fail "Share after the restart"
record '{"step":"redials-after-daemon-restart"}'

kill "$HOLD"
wait_for 5 "the phone ignored SIGTERM" exited "$HOLD"
wait "$HOLD" || true
wait_for 3 "a stopped phone still shows connected" disconnected
record '{"step":"stopped-phone-disconnects"}'

RECEIVED=$(signal_count "$ID")
OUTPUT=$(phone --transcript "$OUT/hostile.jsonl" share --unchecked --kind link --text "ftp://example.org/")
echo "$OUTPUT" | grep -q '"accepted":false.*protocol error' || fail "desktop accepted an ftp link: $OUTPUT"
OUTPUT=$(phone --transcript "$OUT/hostile.jsonl" share --unchecked --kind text --text "$(head -c 61441 /dev/zero | tr '\0' a)")
echo "$OUTPUT" | grep -q '"accepted":false.*protocol error' || fail "desktop accepted an oversized text: $OUTPUT"
[[ $(signal_count "$ID") == "$RECEIVED" ]] || fail "a hostile share reached D-Bus"
grep -q "share message violates the schema" "$OUT/linkd.log" || fail "desktop did not log the violation"
if "$BIN/umbriel-link-phone" check-transcript "$ROOT/protocol/link-v1/messages.cddl" "$OUT/hostile.jsonl" 2> /dev/null; then
  fail "the schema accepts the hostile shares"
fi
record '{"step":"hostile-phone-closed","code":"protocol-error","schema_rejects":true}'

OUTPUT=$(phone --transcript "$OUT/transcript.jsonl" share --kind text --text "on demand") || fail "on-demand share"
record "$(echo "$OUTPUT" | sed 's/^{/{"step":"on-demand-share",/')"

CHECK=$("$BIN/umbriel-link-phone" check-transcript "$ROOT/protocol/link-v1/messages.cddl" "$OUT/transcript.jsonl") \
  || fail "transcript violates the schema"
record "$(echo "$CHECK" | sed 's/^{/{"step":"transcript-schema",/')"
cat "$OUT/results.jsonl"
echo "PASS; artifacts: $OUT"
