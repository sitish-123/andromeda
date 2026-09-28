#!/usr/bin/env bash
# Link through the desktop's host firewall with no port opened: the desktop's namespace drops every incoming packet
# but established ones (as ufw or firewalld do), the phone sits behind a veth pair, and RFCOMM is the Unix socket
# stand-in from link_bluetooth.sh.
#
# Proves, with the firewall up:
# 1. The firewall drops the phone's dial: connect falls back to Bluetooth and the drop rule counts packets.
# 2. A held phone on Bluetooth asks for a punch, the desktop sends it from the Link port, and the session moves to
#    QUIC by itself, well before the 30 s probe interval.
# 3. Shares both ways over the moved session, and a 4 MiB file with matching SHA-256.
# Every control message is validated against protocol/link-v1/messages.cddl.
# Writes results.jsonl, hold.jsonl, nft.txt, transcript.jsonl, linkd.log, hold.log to $OUT (default
# ./artifacts/link-firewall).
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
OUT=${OUT:-$(pwd)/artifacts/link-firewall}
BIN=${LINK_BIN:-$ROOT/link/target/debug}
if [[ -z ${LINK_IN_NS:-} ]]; then
  rm -rf "$OUT"
  mkdir -p "$OUT"
  exec env LINK_IN_NS=1 OUT="$OUT" BIN="$BIN" unshare --user --map-root-user --net bash "$0"
fi

RUNTIME=$(mktemp -d /tmp/link-firewall.XXXX)
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

RFCOMM=$RUNTIME/rfcomm.sock
DOWNLOADS=$RUNTIME/downloads
PHONE_DL=$RUNTIME/phone-downloads
mkdir -p "$DOWNLOADS" "$PHONE_DL" "$RUNTIME/src"
link() { gdbus call --session -d org.umbriel.Link1 -o /org/umbriel/Link1 -m "org.umbriel.Link1.$1" "${@:2}"; }
devices() { gdbus call --session -d org.umbriel.Link1 -o /org/umbriel/Link1 -m org.freedesktop.DBus.Properties.Get org.umbriel.Link1 Devices; }
connected() { devices | grep -q "'phone', true"; }
XDG_STATE_HOME=$RUNTIME/desktop XDG_DOWNLOAD_DIR=$DOWNLOADS UMBRIEL_LINK_TEST_BLUETOOTH_SOCKET=$RFCOMM \
  RUST_LOG=info "$BIN/umbriel-linkd" >> "$OUT/linkd.log" 2>&1 &
for _ in $(seq 100); do link CancelPairing > /dev/null 2>&1 && break; sleep 0.05; done
link CancelPairing > /dev/null || fail "umbriel-linkd did not come up"
PORT=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["port"])' "$RUNTIME/desktop/umbriel-link/devices.json")
phone() {
  in_phone "$BIN/umbriel-link-phone" --state "$RUNTIME/phone" --name phone --downloads "$PHONE_DL" \
    --transcript "$OUT/transcript.jsonl" --bluetooth-socket "$RFCOMM" "$@"
}
events() { grep -c "\"event\":\"$1\"" "$OUT/hold.jsonl" || true; }
say() { printf '%s\n' "$*" >&3; }
sha() { sha256sum < "$1" | cut -d' ' -f1; }
dropped() { nft -j list chain inet firewall input | python3 -c '
import json, sys
rules = [item["rule"] for item in json.load(sys.stdin)["nftables"] if "rule" in item]
print(sum(expr["counter"]["packets"] for rule in rules for expr in rule["expr"] if "counter" in expr))'; }

CODE=$(link StartPairing | sed -E "s/^\('([0-9]+)', .*/\1/")
phone pair --code "$CODE" --addr "10.78.0.1:$PORT" > /dev/null || fail "pairing"
ID=$(devices | grep -o "'[0-9a-f]\{32\}', 'phone'" | cut -d"'" -f2)
[[ -n $ID ]] || fail "desktop does not list the phone: $(devices)"
sleep 1

nft -f - <<'NFT'
table inet firewall {
  chain input {
    type filter hook input priority filter; policy drop;
    iif lo accept
    ct state established,related accept
    counter drop
  }
}
NFT
OUTPUT=$(phone connect) || fail "connect behind the firewall: $OUTPUT"
echo "$OUTPUT" | grep -q '"via":"bluetooth"' || fail "the firewall let the dial through: $OUTPUT"
[[ $(dropped) -gt 0 ]] || fail "the drop rule counted nothing"
record "{\"step\":\"firewall-drops-the-dial\",\"dropped_packets\":$(dropped)}"

mkfifo "$RUNTIME/phone.in"
exec 3<> "$RUNTIME/phone.in"
START=$(date +%s)
RUST_LOG=info nsenter -t "$PHONE_NS" -n -- "$BIN/umbriel-link-phone" --state "$RUNTIME/phone" --name phone \
  --downloads "$PHONE_DL" --transcript "$OUT/transcript.jsonl" --bluetooth-socket "$RFCOMM" \
  hold --on-offer accept < "$RUNTIME/phone.in" > "$OUT/hold.jsonl" 2> "$OUT/hold.log" &
wait_for 30 "the held phone never connected over Bluetooth" eval '[[ $(events connected) -ge 1 ]]'
grep '"event":"connected"' "$OUT/hold.jsonl" | head -1 | grep -q '"via":"bluetooth"' || fail "hold did not start on Bluetooth"
wait_for 25 "the session never moved through the firewall" eval '[[ $(events connected) -ge 2 ]]'
MOVED=$(( $(date +%s) - START ))
grep '"event":"connected"' "$OUT/hold.jsonl" | tail -1 | grep -q '"via":"last-known"' || fail "the move did not use the IP path"
grep -q "punching toward \[10.78.0.2:" "$OUT/linkd.log" || fail "the desktop never punched"
wait_for 5 "D-Bus does not show the phone after the move" connected
record "{\"step\":\"punch-moves-to-quic\",\"seconds\":$MOVED}"

link Share "$ID" text "'through the firewall'" > /dev/null || fail "Share after the move"
wait_for 5 "the phone missed the share" grep -q '"text":"through the firewall"' "$OUT/hold.jsonl"
link SetAutoAccept "'$ID'" true > /dev/null || fail "SetAutoAccept"
head -c 4194304 /dev/urandom > "$RUNTIME/src/up.bin"
say "send $RUNTIME/src/up.bin"
wait_for 30 "the phone's file never reached the desktop" test -f "$DOWNLOADS/up.bin"
[[ $(sha "$RUNTIME/src/up.bin") == $(sha "$DOWNLOADS/up.bin") ]] || fail "the phone's file differs on the desktop"
[[ $(events connected) == 2 ]] || fail "the session left QUIC during the transfer"
record '{"step":"shares-and-files-over-quic","bytes":4194304,"sha256_match":true}'

nft list ruleset > "$OUT/nft.txt"
CHECK=$("$BIN/umbriel-link-phone" check-transcript "$ROOT/protocol/link-v1/messages.cddl" "$OUT/transcript.jsonl") \
  || fail "transcript violates the schema"
# 6570756e6368: the CBOR text "punch".
grep -q 6570756e6368 "$OUT/transcript.jsonl" || fail "no punch in the transcript"
record "$(echo "$CHECK" | sed 's/^{/{"step":"transcript-schema",/')"
cat "$OUT/results.jsonl"
echo "PASS; artifacts: $OUT"
