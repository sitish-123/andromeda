#!/usr/bin/env bash
# Link's hotspot handoff without radios: the phone reaches the desktop over the Bluetooth stand-in (a Unix socket, see
# link_bluetooth.sh), its hotspot is --hotspot's fixed credentials, and NetworkManager is nm_mock.py, which "joins" by
# bringing up a second veth pair (10.80.0.0/24) between the namespaces. The home path (10.81.0.0/24) is cut with 100%
# loss for the whole run, so full speed can only come through the hotspot.
#
# Proves:
# 1. A phone send over the Bluetooth limit starts the hotspot and hands its credentials over Bluetooth; the desktop
#    joins with exactly those (WPA-PSK, not autoconnected, volatile), reports its address, the session moves to it
#    (via "hotspot"), and the 32 MiB file arrives with a matching SHA-256. D-Bus signals Hotspot with the SSID.
# 2. With nothing moving for a minute, the phone ends the hotspot: the desktop deactivates the connection, signals
#    Hotspot with an empty SSID, and the phone is back on Bluetooth within seconds, not after the idle timeout.
# 3. A desktop SendFiles over the limit asks the phone for its hotspot, waits for the move, then sends; 24 MiB
#    arrives with a matching SHA-256.
# 4. A join NetworkManager fails ends the attempt on both sides with its reason; the phone's send over the Bluetooth
#    limit fails with it and nothing is sent, while a 2 MiB send (past the 1 MiB upgrade threshold, within the limit)
#    asks for the hotspot, then goes over Bluetooth when the join fails, in both directions. A 512 KiB send never asks for it.
# 5. The hotspot's address is not stored as a last-known address.
# Every control message is validated against protocol/link-v1/messages.cddl.
# Writes results.jsonl, hold.jsonl, nm.jsonl, signals.txt, transcript.jsonl, linkd.log to $OUT (default
# ./artifacts/link-hotspot).
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
OUT=${OUT:-$(pwd)/artifacts/link-hotspot}
BIN=${LINK_BIN:-$ROOT/link/target/debug}
if [[ -z ${LINK_IN_NS:-} ]]; then
  rm -rf "$OUT"
  mkdir -p "$OUT"
  exec env LINK_IN_NS=1 OUT="$OUT" BIN="$BIN" unshare --user --map-root-user --net bash "$0"
fi

RUNTIME=$(mktemp -d /tmp/link-hotspot.XXXX)
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
ip addr add 10.81.0.1/24 dev d0
ip link set d0 up
in_phone() { nsenter -t "$PHONE_NS" -n -- "$@"; }
in_phone ip link set lo up
in_phone ip addr add 10.81.0.2/24 dev p0
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

JOIN="ip link add h0 type veth peer name hp0 && ip link set hp0 netns $PHONE_NS && ip addr add 10.80.0.1/24 dev h0 \
  && ip link set h0 up && nsenter -t $PHONE_NS -n ip addr add 10.80.0.2/24 dev hp0 \
  && nsenter -t $PHONE_NS -n ip link set hp0 up"
python3 "$ROOT/tests/e2e/nm_mock.py" "$OUT/nm.jsonl" "$JOIN" "ip link del h0" "$RUNTIME/nm-fail" 2> "$OUT/nm.log" &
wait_for 5 "the NetworkManager stand-in did not come up" \
  gdbus introspect --system -d org.freedesktop.NetworkManager -o /org/freedesktop/NetworkManager > /dev/null

RFCOMM=$RUNTIME/rfcomm.sock
DOWNLOADS=$RUNTIME/downloads
PHONE_DL=$RUNTIME/phone-downloads
mkdir -p "$DOWNLOADS" "$PHONE_DL" "$RUNTIME/src"
link() { gdbus call --session -d org.umbriel.Link1 -o /org/umbriel/Link1 -m "org.umbriel.Link1.$1" "${@:2}"; }
devices() { gdbus call --session -d org.umbriel.Link1 -o /org/umbriel/Link1 -m org.freedesktop.DBus.Properties.Get org.umbriel.Link1 Devices; }
XDG_STATE_HOME=$RUNTIME/desktop XDG_DOWNLOAD_DIR=$DOWNLOADS UMBRIEL_LINK_TEST_BLUETOOTH_SOCKET=$RFCOMM \
  RUST_LOG=info "$BIN/umbriel-linkd" >> "$OUT/linkd.log" 2>&1 &
for _ in $(seq 100); do link CancelPairing > /dev/null 2>&1 && break; sleep 0.05; done
link CancelPairing > /dev/null || fail "umbriel-linkd did not come up"
PORT=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["port"])' "$RUNTIME/desktop/umbriel-link/devices.json")
events() { grep -c "\"event\":\"$1\"" "$OUT/hold.jsonl" || true; }
last_via() { grep '"event":"connected"' "$OUT/hold.jsonl" | tail -1 | grep -o '"via":"[a-z-]*"' | cut -d'"' -f4; }
nm_calls() { grep -c "\"call\": \"$1\"" "$OUT/nm.jsonl" 2>/dev/null || true; }
say() { printf '%s\n' "$*" >&3; }
sha() { sha256sum < "$1" | cut -d' ' -f1; }

CODE=$(link StartPairing | sed -E "s/^\('([0-9]+)', .*/\1/")
in_phone "$BIN/umbriel-link-phone" --state "$RUNTIME/phone" --name phone pair --code "$CODE" --addr "10.81.0.1:$PORT" \
  > /dev/null || fail "pairing"
ID=$(devices | grep -o "'[0-9a-f]\{32\}', 'phone'" | cut -d"'" -f2)
[[ -n $ID ]] || fail "desktop does not list the phone: $(devices)"
link SetAutoAccept "'$ID'" true > /dev/null || fail "SetAutoAccept"
in_phone tc qdisc add dev p0 root netem loss 100%

dbus-monitor --session "type='signal',interface='org.umbriel.Link1',member='Hotspot'" > "$OUT/signals.txt" 2>&1 &
wait_for 5 "dbus-monitor did not start" test -s "$OUT/signals.txt"
mkfifo "$RUNTIME/phone.in"
exec 3<> "$RUNTIME/phone.in"
RUST_LOG=info nsenter -t "$PHONE_NS" -n -- "$BIN/umbriel-link-phone" --state "$RUNTIME/phone" --name phone \
  --downloads "$PHONE_DL" --transcript "$OUT/transcript.jsonl" --bluetooth-socket "$RFCOMM" \
  --hotspot "UmbrielTest:secret pass 1" hold --on-offer accept < "$RUNTIME/phone.in" > "$OUT/hold.jsonl" \
  2> "$OUT/hold.log" &
wait_for 30 "the held phone never connected over Bluetooth" eval '[[ $(last_via) == bluetooth ]]'

head -c 33554432 /dev/urandom > "$RUNTIME/src/up.bin"
say "send $RUNTIME/src/up.bin"
wait_for 30 "the session never moved to the hotspot" eval '[[ $(last_via) == hotspot ]]'
grep -q "hotspot started" "$OUT/hold.log" || fail "the phone did not start its hotspot"
python3 - "$OUT/nm.jsonl" <<'PY' || fail "the desktop joined with other settings: $(cat "$OUT/nm.jsonl")"
import json, sys
join = [json.loads(line) for line in open(sys.argv[1])][0]
del join["t"]
assert join == {"call": "add-and-activate", "ssid": "UmbrielTest", "psk": "secret pass 1", "key_mgmt": "wpa-psk",
                "autoconnect": False, "persist": "volatile", "fails": False}, join
PY
wait_for 60 "the phone's 32 MiB never reached the desktop" test -f "$DOWNLOADS/up.bin"
[[ $(sha "$RUNTIME/src/up.bin") == $(sha "$DOWNLOADS/up.bin") ]] || fail "the phone's file differs on the desktop"
grep -A2 'member=Hotspot' "$OUT/signals.txt" | grep -q 'string "UmbrielTest"' || fail "no Hotspot signal with the SSID"
record '{"step":"phone-send-moves-to-hotspot","bytes":33554432,"sha256_match":true}'

wait_for 90 "the phone never ended the idle hotspot" grep -q "hotspot stopped" "$OUT/hold.log"
date +%s.%N > "$OUT/stopped-at.txt"
wait_for 10 "the desktop did not leave the hotspot" eval '[[ $(nm_calls deactivate) == 1 ]]'
wait_for 20 "the phone was not back on Bluetooth soon after" eval '[[ $(last_via) == bluetooth ]]'
wait_for 5 "no Hotspot signal for leaving" eval '[[ $(grep -A2 "member=Hotspot" "$OUT/signals.txt" | grep -c "string \"\"") -ge 1 ]]'
record '{"step":"idle-hotspot-ends-back-on-bluetooth"}'

head -c 25165824 /dev/urandom > "$RUNTIME/src/down.bin"
python3 "$ROOT/tests/e2e/link_send_files.py" "$ID" "$RUNTIME/src/down.bin" > /dev/null || fail "SendFiles over the limit"
[[ $(last_via) == hotspot ]] || fail "SendFiles returned before the session moved to the hotspot"
wait_for 60 "the desktop's 24 MiB never reached the phone" test -f "$PHONE_DL/down.bin"
[[ $(sha "$RUNTIME/src/down.bin") == $(sha "$PHONE_DL/down.bin") ]] || fail "the desktop's file differs on the phone"
record '{"step":"desktop-send-asks-for-hotspot","bytes":25165824,"sha256_match":true}'

wait_for 90 "the second hotspot never ended" eval '[[ $(nm_calls deactivate) == 2 ]]'
wait_for 20 "the phone was not back on Bluetooth after the second hotspot" eval '[[ $(last_via) == bluetooth ]]'
touch "$RUNTIME/nm-fail"
FAILED=$(events send-failed)
say "send $RUNTIME/src/up.bin"
wait_for 60 "a failed join did not fail the send" eval '(( $(events send-failed) > FAILED ))'
grep '"event":"send-failed"' "$OUT/hold.jsonl" | tail -1 | grep -q 'NetworkManager could not join it' \
  || fail "the failed send does not carry NetworkManager's reason"
grep -q '"fails": true' "$OUT/nm.jsonl" || fail "the desktop did not try the failing join"
record '{"step":"failed-join-ends-both-sides","reason":"NetworkManager could not join it"}'

head -c 2097152 /dev/urandom > "$RUNTIME/src/small.bin"
JOINS=$(nm_calls add-and-activate)
say "send $RUNTIME/src/small.bin"
wait_for 60 "a 2 MiB send whose hotspot failed never arrived over Bluetooth" test -f "$DOWNLOADS/small.bin"
[[ $(sha "$RUNTIME/src/small.bin") == $(sha "$DOWNLOADS/small.bin") ]] || fail "the 2 MiB file differs on the desktop"
(( $(nm_calls add-and-activate) > JOINS )) || fail "a 2 MiB send over Bluetooth did not ask for the hotspot"
[[ $(last_via) == bluetooth ]] || fail "the 2 MiB fallback did not stay on Bluetooth"
record '{"step":"small-send-tries-hotspot-then-bluetooth","bytes":2097152,"sha256_match":true}'
FALLBACKS=$(grep -c "hub\] .*sending over Bluetooth: hotspot" "$OUT/linkd.log" || true)
python3 "$ROOT/tests/e2e/link_send_files.py" "$ID" "$RUNTIME/src/small.bin" > /dev/null \
  || fail "a desktop 2 MiB SendFiles whose hotspot failed was refused"
wait_for 60 "the desktop's 2 MiB never reached the phone over Bluetooth" test -f "$PHONE_DL/small.bin"
[[ $(sha "$RUNTIME/src/small.bin") == $(sha "$PHONE_DL/small.bin") ]] || fail "the desktop's 2 MiB differs on the phone"
(( $(grep -c "hub\] .*sending over Bluetooth: hotspot" "$OUT/linkd.log") > FALLBACKS )) \
  || fail "a desktop 2 MiB send over Bluetooth did not ask for the hotspot first"
record '{"step":"desktop-small-send-tries-hotspot-then-bluetooth","bytes":2097152,"sha256_match":true}'

head -c 524288 /dev/urandom > "$RUNTIME/src/tiny.bin"
JOINS=$(nm_calls add-and-activate)
say "send $RUNTIME/src/tiny.bin"
wait_for 30 "a 512 KiB send never arrived over Bluetooth" test -f "$DOWNLOADS/tiny.bin"
[[ $(sha "$RUNTIME/src/tiny.bin") == $(sha "$DOWNLOADS/tiny.bin") ]] || fail "the 512 KiB file differs on the desktop"
(( $(nm_calls add-and-activate) == JOINS )) || fail "a 512 KiB send asked for the hotspot"
record '{"step":"tiny-send-stays-on-bluetooth","bytes":524288,"sha256_match":true}'

STORED=$(python3 -c 'import json,sys; print(" ".join(json.load(open(sys.argv[1]))["peers"][0]["addresses"]))' "$RUNTIME/phone/devices.json")
[[ $STORED != *10.80.0.* ]] || fail "the hotspot's address was stored: $STORED"
record '{"step":"hotspot-address-not-stored"}'

CHECK=$("$BIN/umbriel-link-phone" check-transcript "$ROOT/protocol/link-v1/messages.cddl" "$OUT/transcript.jsonl") \
  || fail "transcript violates the schema"
record "$(echo "$CHECK" | sed 's/^{/{"step":"transcript-schema",/')"
cat "$OUT/results.jsonl"
echo "PASS; artifacts: $OUT"
