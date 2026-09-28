#!/usr/bin/env bash
# Link over Bluetooth without radios: the daemon's RFCOMM side is replaced by a Unix socket
# (UMBRIEL_LINK_TEST_BLUETOOTH_SOCKET) and the phone reaches it with --bluetooth-socket, so everything after the
# descriptor handover (TLS over the byte stream, the stream multiplexer, the session, transfers) is the real code.
# The phone sits in its own network namespace behind a veth pair; 100% loss on it cuts every IP path.
#
# Proves, with the IP path cut:
# 1. The desktop's hello carries its Bluetooth address, and the phone stores it at pairing.
# 2. connect falls back to Bluetooth (via "bluetooth", no address) and D-Bus shows the phone connected.
# 3. Shares both ways, and files both ways with matching SHA-256, over the stream multiplexer.
# 4. More than 20 MiB, with a phone that cannot start a hotspot (link_hotspot.sh covers one that can), is refused
#    from D-Bus SendFiles and from the phone before any byte moves.
# 5. The IP path returning moves the held session from Bluetooth to QUIC by itself, with no lost share.
# 6. A phone whose key is not paired is refused over Bluetooth and nothing is delivered.
# 7. Garbage on the Bluetooth socket fails the handshake and the daemon keeps serving.
# Every control message is validated against protocol/link-v1/messages.cddl.
# Writes results.jsonl, hold.jsonl, signals.txt, transcript.jsonl, linkd.log to $OUT (default
# ./artifacts/link-bluetooth).
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
OUT=${OUT:-$(pwd)/artifacts/link-bluetooth}
BIN=${LINK_BIN:-$ROOT/link/target/debug}
if [[ -z ${LINK_IN_NS:-} ]]; then
  rm -rf "$OUT"
  mkdir -p "$OUT"
  exec env LINK_IN_NS=1 OUT="$OUT" BIN="$BIN" unshare --user --map-root-user --net bash "$0"
fi

RUNTIME=$(mktemp -d /tmp/link-bluetooth.XXXX)
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

CODE=$(link StartPairing | sed -E "s/^\('([0-9]+)', .*/\1/")
phone pair --code "$CODE" --addr "10.79.0.1:$PORT" > /dev/null || fail "pairing"
ID=$(devices | grep -o "'[0-9a-f]\{32\}', 'phone'" | cut -d"'" -f2)
[[ -n $ID ]] || fail "desktop does not list the phone: $(devices)"
STORED=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["peers"][0].get("bluetooth"))' "$RUNTIME/phone/devices.json")
[[ $STORED == 00:00:00:00:00:01 ]] || fail "the phone stored Bluetooth address $STORED"
record '{"step":"hello-carries-bluetooth","address":"00:00:00:00:00:01"}'

blackhole
OUTPUT=$(phone connect) || fail "connect with the IP path cut: $OUTPUT"
echo "$OUTPUT" | grep -q '"via":"bluetooth"' || fail "connect did not use Bluetooth: $OUTPUT"
echo "$OUTPUT" | grep -q '"addr":null' || fail "a Bluetooth session reported an address: $OUTPUT"
record '{"step":"connect-falls-back-to-bluetooth"}'

dbus-monitor --session "type='signal',interface='org.umbriel.Link1'" > "$OUT/signals.txt" 2>&1 &
wait_for 5 "dbus-monitor did not start" test -s "$OUT/signals.txt"
mkfifo "$RUNTIME/phone.in"
exec 3<> "$RUNTIME/phone.in"
RUST_LOG=info nsenter -t "$PHONE_NS" -n -- "$BIN/umbriel-link-phone" --state "$RUNTIME/phone" --name phone \
  --downloads "$PHONE_DL" --transcript "$OUT/transcript.jsonl" --bluetooth-socket "$RFCOMM" \
  hold --on-offer accept < "$RUNTIME/phone.in" > "$OUT/hold.jsonl" 2> "$OUT/hold.log" &
wait_for 30 "the held phone never connected over Bluetooth" eval '[[ $(events connected) == 1 ]]'
grep '"event":"connected"' "$OUT/hold.jsonl" | grep -q '"via":"bluetooth"' || fail "hold did not connect over Bluetooth"
wait_for 5 "D-Bus does not show the Bluetooth phone connected: $(devices)" connected
record '{"step":"held-over-bluetooth"}'

link Share "$ID" text "'over the air ✓'" > /dev/null || fail "D-Bus Share over Bluetooth"
wait_for 5 "the phone missed the desktop's text" grep -q '"text":"over the air ✓"' "$OUT/hold.jsonl"
say "text hello over bluetooth"
wait_for 10 "no Received signal for the phone's text" grep -qF 'hello over bluetooth' "$OUT/signals.txt"
record '{"step":"shares-both-ways"}'

head -c 3145728 /dev/urandom > "$RUNTIME/src/down.bin"
python3 "$ROOT/tests/e2e/link_send_files.py" "$ID" "$RUNTIME/src/down.bin" > /dev/null || fail "SendFiles over Bluetooth"
wait_for 60 "the desktop's file never reached the phone" test -f "$PHONE_DL/down.bin"
[[ $(sha "$RUNTIME/src/down.bin") == $(sha "$PHONE_DL/down.bin") ]] || fail "the desktop's file differs on the phone"
link SetAutoAccept "'$ID'" true > /dev/null || fail "SetAutoAccept"
head -c 2097152 /dev/urandom > "$RUNTIME/src/up.bin"
say "send $RUNTIME/src/up.bin"
wait_for 60 "the phone's file never reached the desktop" test -f "$DOWNLOADS/up.bin"
[[ $(sha "$RUNTIME/src/up.bin") == $(sha "$DOWNLOADS/up.bin") ]] || fail "the phone's file differs on the desktop"
record '{"step":"files-both-ways","bytes":[3145728,2097152],"sha256_match":true}'

head -c 22020096 /dev/zero > "$RUNTIME/src/big.bin"
OUTPUT=$(python3 "$ROOT/tests/e2e/link_send_files.py" "$ID" "$RUNTIME/src/big.bin" 2>&1) && fail "21 MiB went over Bluetooth"
[[ $OUTPUT == *"this phone cannot start a hotspot"* ]] || fail "SendFiles of 21 MiB: $OUTPUT"
say "send $RUNTIME/src/big.bin"
wait_for 10 "the phone sent 21 MiB over Bluetooth" eval '[[ $(events send-failed) == 1 ]]'
grep '"event":"send-failed"' "$OUT/hold.jsonl" | grep -q 'too large to send over Bluetooth' || fail "the phone refused 21 MiB for another reason"
[[ ! -e $DOWNLOADS/big.bin && ! -e $PHONE_DL/big.bin ]] || fail "part of the refused file arrived"
record '{"step":"bluetooth-file-limit","bytes":22020096,"refused_by":["dbus","phone"]}'

clear_path
wait_for 60 "the session never moved from Bluetooth to IP" eval '[[ $(events connected) == 2 ]]'
grep '"event":"connected"' "$OUT/hold.jsonl" | tail -1 | grep -q '"via":"last-known"' || fail "the move did not use the IP path"
wait_for 5 "D-Bus does not show the phone after the move" connected
link Share "$ID" text "'back on wifi'" > /dev/null || fail "Share after moving to IP"
wait_for 5 "the phone missed the share after the move" grep -q '"text":"back on wifi"' "$OUT/hold.jsonl"
record '{"step":"moves-up-to-ip"}'

blackhole
mkdir -p "$RUNTIME/stranger"
cp "$RUNTIME/phone/devices.json" "$RUNTIME/stranger/devices.json"
RECEIVED=$(grep -c 'member=Received' "$OUT/signals.txt" || true)
OUTPUT=$(in_phone "$BIN/umbriel-link-phone" --state "$RUNTIME/stranger" --name stranger --bluetooth-socket "$RFCOMM" \
  share --kind text --text "from a stranger" 2>&1) && fail "an unpaired phone shared over Bluetooth: $OUTPUT"
[[ $(grep -c 'member=Received' "$OUT/signals.txt" || true) == "$RECEIVED" ]] || fail "the stranger's share reached D-Bus"
devices | grep -q stranger && fail "the desktop lists the stranger"
record '{"step":"unpaired-key-refused"}'

python3 - "$RFCOMM" <<'PY'
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sys.argv[1])
s.sendall(b"\x16\x03\x01\xff\xff" + b"not a handshake" * 100)
s.close()
PY
sleep 1
link CancelPairing > /dev/null || fail "the daemon stopped answering after garbage on the Bluetooth socket"
grep -q "Bluetooth: handshake failed" "$OUT/linkd.log" || fail "the daemon did not log the failed handshake"
record '{"step":"garbage-handshake-survived"}'

CHECK=$("$BIN/umbriel-link-phone" check-transcript "$ROOT/protocol/link-v1/messages.cddl" "$OUT/transcript.jsonl") \
  || fail "transcript violates the schema"
record "$(echo "$CHECK" | sed 's/^{/{"step":"transcript-schema",/')"
cat "$OUT/results.jsonl"
echo "PASS; artifacts: $OUT"
