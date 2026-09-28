#!/usr/bin/env bash
# Phone status and per-feature rate limits across two network namespaces. Proves: a present phone's battery, charging,
# and network appear in D-Bus DeviceStatus on connect; a change inside the phone's 10 s window arrives when it ends,
# not at once; the entry goes when the phone disconnects; a hostile phone flooding shares, file offers, clipboard
# offers, and statuses gets exactly its burst through (10 shares acked and signalled, 5 offers signalled and the rest
# answered busy, 20 clipboard offers signalled, 3 statuses applied) and the rest dropped. Every message is validated
# against protocol/link-v1/messages.cddl. Writes results.jsonl, signals.txt, transcript.jsonl, hold.jsonl, and
# linkd.log to $OUT (default ./artifacts/link-status).
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
OUT=${OUT:-$(pwd)/artifacts/link-status}
BIN=${LINK_BIN:-$ROOT/link/target/debug}
if [[ -z ${LINK_IN_NS:-} ]]; then
  rm -rf "$OUT"
  mkdir -p "$OUT"
  exec env LINK_IN_NS=1 OUT="$OUT" BIN="$BIN" unshare --user --map-root-user --net bash "$0"
fi

RUNTIME=$(mktemp -d /tmp/link-status.XXXX)
trap 'kill $(jobs -p) 2>/dev/null || true; wait 2>/dev/null; rm -rf "$RUNTIME"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }
record() { printf '%s\n' "$1" >> "$OUT/results.jsonl"; }
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
prop() { gdbus call --session -d org.umbriel.Link1 -o /org/umbriel/Link1 -m org.freedesktop.DBus.Properties.Get org.umbriel.Link1 "$1"; }
devices() { prop Devices; }
connected() { devices | grep -q "'phone', true"; }
disconnected() { devices | grep -q "'phone', false"; }
phone() { in_phone "$BIN/umbriel-link-phone" --state "$RUNTIME/phone" --name phone --transcript "$OUT/transcript.jsonl" "$@"; }
say() { printf '%s\n' "$*" >&3; }
signals() { grep -c "member=$1" "$OUT/signals.txt" || true; }
dropped() { grep -c "dropping a $1 over its rate limit" "$OUT/linkd.log" || true; }

XDG_STATE_HOME=$RUNTIME/desktop XDG_DOWNLOAD_DIR=$RUNTIME/downloads "$BIN/umbriel-linkd" > "$OUT/linkd.log" 2>&1 &
for _ in $(seq 100); do link CancelPairing > /dev/null 2>&1 && break; sleep 0.05; done
PORT=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["port"])' "$RUNTIME/desktop/umbriel-link/devices.json")
CODE=$(link StartPairing | sed -E "s/^\('([0-9]+)', .*/\1/")
phone pair --code "$CODE" --addr "10.78.0.1:$PORT" > /dev/null || fail "pairing"
ID=$(devices | grep -o "'[0-9a-f]\{32\}', 'phone'" | cut -d"'" -f2)
dbus-monitor --session "type='signal',interface='org.umbriel.Link1'" > "$OUT/signals.txt" 2>&1 &
wait_for 5 "dbus-monitor did not start" test -s "$OUT/signals.txt"

mkfifo "$RUNTIME/phone.in"
exec 3<> "$RUNTIME/phone.in"
nsenter -t "$PHONE_NS" -n -- "$BIN/umbriel-link-phone" --state "$RUNTIME/phone" --name phone \
  --transcript "$OUT/transcript.jsonl" hold --status 80,0,wifi < "$RUNTIME/phone.in" > "$OUT/hold.jsonl" 2> "$OUT/hold.log" &
HOLD=$!
wait_for 10 "present phone never shown connected" connected
wait_for 5 "DeviceStatus never showed the phone: $(prop DeviceStatus)" \
  eval "prop DeviceStatus | grep -qF \"'$ID': (uint32 80, false, 'wifi')\""
record '{"step":"status-on-connect","battery":80,"charging":false,"network":"wifi"}'

say "status 79 1 cellular"
CHANGED=$(date +%s.%N)
sleep 2 # real time: inside the phone's 10 s window, where the change must not have gone out yet
prop DeviceStatus | grep -qF "(uint32 80, false, 'wifi')" || fail "a change went out inside the window: $(prop DeviceStatus)"
wait_for 15 "the changed status never arrived: $(prop DeviceStatus)" \
  eval "prop DeviceStatus | grep -qF \"'$ID': (uint32 79, true, 'cellular')\""
DELAY=$(python3 -c "import time,sys; print(round(time.time() - float(sys.argv[1]), 1))" "$CHANGED")
record "{\"step\":\"status-change-rate-limited\",\"arrived_after_seconds\":$DELAY}"

kill "$HOLD"; wait "$HOLD" 2> /dev/null || true
wait_for 5 "a stopped phone still shows connected" disconnected
! prop DeviceStatus | grep -qF "$ID" || fail "DeviceStatus outlived the session: $(prop DeviceStatus)"
record '{"step":"status-cleared-on-disconnect"}'

BEFORE=$(signals Received)
OUTPUT=$(phone flood --kind share --count 50)
echo "$OUTPUT" | grep -q '"acked":10' || fail "share flood: $OUTPUT"
wait_for 5 "Received does not show the burst" eval '(( $(signals Received) - BEFORE == 10 ))'
[[ $(dropped share) == 40 ]] || fail "dropped $(dropped share) shares, not 40"
record "$(echo "$OUTPUT" | sed 's/^{/{"step":"share-flood",/')"

BEFORE=$(signals TransferOffered)
OUTPUT=$(phone flood --kind offer --count 20)
echo "$OUTPUT" | grep -q '"offers_busy":15' || fail "offer flood: $OUTPUT"
[[ $(( $(signals TransferOffered) - BEFORE )) == 5 ]] || fail "TransferOffered for $(( $(signals TransferOffered) - BEFORE )) offers, not 5"
record "$(echo "$OUTPUT" | sed 's/^{/{"step":"offer-flood",/')"

BEFORE=$(signals ClipboardOffered)
phone flood --kind clip --count 60 > /dev/null
wait_for 5 "ClipboardOffered does not show the burst" eval '(( $(signals ClipboardOffered) - BEFORE == 20 ))'
[[ $(dropped clip-offer) == 40 ]] || fail "dropped $(dropped clip-offer) clip offers, not 40"
record '{"step":"clip-flood","sent":60,"signalled":20}'

phone flood --kind status --count 20 > /dev/null
[[ $(dropped status) == 17 ]] || fail "dropped $(dropped status) statuses, not 17"
record '{"step":"status-flood","sent":20,"applied":3}'

CHECK=$("$BIN/umbriel-link-phone" check-transcript "$ROOT/protocol/link-v1/messages.cddl" "$OUT/transcript.jsonl") \
  || fail "transcript violates the schema"
record "$(echo "$CHECK" | sed 's/^{/{"step":"transcript-schema",/')"
cat "$OUT/results.jsonl"
echo "PASS; artifacts: $OUT"
