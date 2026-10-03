#!/usr/bin/env bash
# Pairing by QR code with no IP path, over Bluetooth: link_bluetooth.sh's set-up (a Unix socket stands in for
# RFCOMM; the phone sits in its own network namespace) with 100% loss on the phone from the start.
#
# Proves:
# 1. The pairing QR code names the desktop's Bluetooth adapter (b=).
# 2. Pairing by that QR code completes over Bluetooth when no address answers; the desktop lists the phone, and the
#    phone's first session is over Bluetooth (a share arrives).
# 3. A QR code with the wrong secret fails over Bluetooth too, and the desktop lists no new phone.
# 4. A QR code with a parameter the phone does not know still parses.
# Every control message is validated against protocol/link-v1/messages.cddl.
# Writes results.jsonl, transcript.jsonl, linkd.log to $OUT (default ./artifacts/link-pair-bluetooth).
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
OUT=${OUT:-$(pwd)/artifacts/link-pair-bluetooth}
BIN=${LINK_BIN:-$ROOT/link/target/debug}
if [[ -z ${LINK_IN_NS:-} ]]; then
  rm -rf "$OUT"
  mkdir -p "$OUT"
  exec env LINK_IN_NS=1 OUT="$OUT" BIN="$BIN" unshare --user --map-root-user --net bash "$0"
fi

RUNTIME=$(mktemp -d /tmp/link-pair-bluetooth.XXXX)
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
ip addr add 10.79.0.1/24 dev d0
ip link set d0 up
in_phone() { nsenter -t "$PHONE_NS" -n -- "$@"; }
in_phone ip link set lo up
in_phone ip addr add 10.79.0.2/24 dev p0
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

RFCOMM=$RUNTIME/rfcomm.sock
DOWNLOADS=$RUNTIME/downloads
PHONE_DL=$RUNTIME/phone-downloads
mkdir -p "$DOWNLOADS" "$PHONE_DL" "$RUNTIME/src"
link() { gdbus call --session -d org.umbriel.Link1 -o /org/umbriel/Link1 -m "org.umbriel.Link1.$1" "${@:2}"; }
devices() { gdbus call --session -d org.umbriel.Link1 -o /org/umbriel/Link1 -m org.freedesktop.DBus.Properties.Get org.umbriel.Link1 Devices; }
connected() { devices | grep -q "'phone', true"; }
XDG_STATE_HOME=$RUNTIME/desktop XDG_DOWNLOAD_DIR=$DOWNLOADS UMBRIEL_LINK_TEST_BLUETOOTH_SOCKET=$RFCOMM \
  "$BIN/umbriel-linkd" >> "$OUT/linkd.log" 2>&1 &
for _ in $(seq 100); do link CancelPairing > /dev/null 2>&1 && break; sleep 0.05; done
link CancelPairing > /dev/null || fail "umbriel-linkd did not come up"
PORT=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["port"])' "$RUNTIME/desktop/umbriel-link/devices.json")
phone() {
  in_phone "$BIN/umbriel-link-phone" --state "$RUNTIME/phone" --name phone --downloads "$PHONE_DL" \
    --transcript "$OUT/transcript.jsonl" --bluetooth-socket "$RFCOMM" "$@"
}
events() { grep -c "\"event\":\"$1\"" "$OUT/hold.jsonl" || true; }
say() { printf '%s\n' "$*" >&3; }
blackhole() { in_phone tc qdisc add dev p0 root netem loss 100%; }
clear_path() { in_phone tc qdisc del dev p0 root; }
sha() { sha256sum < "$1" | cut -d' ' -f1; }


blackhole
URI=$(link StartPairing | sed -E "s/^\('[0-9]+', '([^']+)'\)$/\1/")
[[ $URI == *"&b=00%3A00%3A00%3A00%3A00%3A01"* ]] || fail "the QR code does not name the Bluetooth adapter: $URI"
record '{"step":"qr-names-bluetooth"}'

BAD=$(echo "$URI" | sed -E 's/&c=[^&]+/\&c=AAAAAAAAAAAAAAAAAAAAAA/')
phone pair --uri "$BAD" > "$RUNTIME/bad.out" 2>&1 && fail "a wrong secret paired: $(cat "$RUNTIME/bad.out")"
[[ $(devices) != *"'phone'"* ]] || fail "the desktop lists a phone after a wrong secret: $(devices)"
record '{"step":"wrong-secret-fails-over-bluetooth"}'

URI=$(link StartPairing | sed -E "s/^\('[0-9]+', '([^']+)'\)$/\1/")
OUTPUT=$(phone pair --uri "$URI&z=later") || fail "pairing over Bluetooth: $OUTPUT"
echo "$OUTPUT" | grep -q '"addr":null' || fail "pairing reported an IP address: $OUTPUT"
wait_for 10 "the desktop does not list the phone: $(devices)" eval 'devices | grep -q "'"'"'phone'"'"'"'
record '{"step":"qr-pairs-over-bluetooth","unknown_parameter":true}'

OUTPUT=$(phone connect) || fail "connect after pairing: $OUTPUT"
echo "$OUTPUT" | grep -q '"via":"bluetooth"' || fail "the session did not use Bluetooth: $OUTPUT"
ID=$(devices | grep -o "'[0-9a-f]\{32\}', 'phone'" | cut -d"'" -f2)
dbus-monitor --session "type='signal',interface='org.umbriel.Link1'" > "$RUNTIME/signals.txt" 2>&1 &
wait_for 5 "dbus-monitor did not start" test -s "$RUNTIME/signals.txt"
phone share --kind text --text "hello over bluetooth" > /dev/null || fail "share over Bluetooth"
wait_for 10 "no Received for the share" grep -q 'string "hello over bluetooth"' "$RUNTIME/signals.txt"
record '{"step":"first-session-over-bluetooth"}'

CHECK=$("$BIN/umbriel-link-phone" check-transcript "$ROOT/protocol/link-v1/messages.cddl" "$OUT/transcript.jsonl") \
  || fail "transcript violates the schema"
record "$(echo "$CHECK" | sed 's/^{/{"step":"transcript-schema",/')"
cat "$OUT/results.jsonl"
echo "PASS; artifacts: $OUT"
