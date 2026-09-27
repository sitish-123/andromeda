#!/usr/bin/env bash
# Quick Share inside umbriel-linkd, driven over D-Bus org.umbriel.Link1.QuickShare, with a sender in a second network
# namespace. Proves: hidden by default (no mDNS service); setting Visible advertises it; an offer reaches D-Bus as the
# Offer signal with the sender, PIN, and files, and nothing is written before Accept; Accept saves the file in
# XDG_DOWNLOAD_DIR and emits Finished with its path; Decline reaches the sender as a rejection; visibility survives a
# daemon restart; Visible=false withdraws the service. Writes results.jsonl, signals.txt, linkd.log to $OUT (default
# ./artifacts/quickshare-daemon).
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
OUT=${OUT:-$(pwd)/artifacts/quickshare-daemon}
BIN=${LINK_BIN:-$ROOT/link/target/debug}
if [[ -z ${QS_IN_NS:-} ]]; then
  rm -rf "$OUT"
  mkdir -p "$OUT"
  exec env QS_IN_NS=1 OUT="$OUT" BIN="$BIN" unshare --user --map-root-user --net bash "$0"
fi

RUNTIME=$(mktemp -d /tmp/qsd-e2e.XXXX)
trap 'kill $(jobs -p) 2>/dev/null || true; wait 2>/dev/null; rm -rf "$RUNTIME"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }
record() { printf '%s\n' "$1" >> "$OUT/results.jsonl"; }

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
# The private bus stands in for the system bus too, so the BLE hint fails harmlessly instead of reaching BlueZ.
export DBUS_SESSION_BUS_ADDRESS=unix:path=$RUNTIME/bus DBUS_SYSTEM_BUS_ADDRESS=unix:path=$RUNTIME/bus
export HOME=$RUNTIME/home XDG_CONFIG_HOME=$RUNTIME/home/.config
mkdir -p "$XDG_CONFIG_HOME" "$RUNTIME/home/Incoming" "$RUNTIME/files"
echo 'XDG_DOWNLOAD_DIR="$HOME/Incoming"' > "$XDG_CONFIG_HOME/user-dirs.dirs"
DOWNLOADS=$RUNTIME/home/Incoming
head -c 5000000 /dev/urandom > "$RUNTIME/files/photo.jpg"
echo "decline me" > "$RUNTIME/files/nope.txt"

QS_IFACE=org.umbriel.Link1.QuickShare
prop() { gdbus call --session -d org.umbriel.Link1 -o /org/umbriel/Link1 -m org.freedesktop.DBus.Properties.Get "$QS_IFACE" "$1"; }
set_visible() {
  gdbus call --session -d org.umbriel.Link1 -o /org/umbriel/Link1 -m org.freedesktop.DBus.Properties.Set "$QS_IFACE" Visible "<$1>" > /dev/null
}
qs() { gdbus call --session -d org.umbriel.Link1 -o /org/umbriel/Link1 -m "$QS_IFACE.$1" "${@:2}" > /dev/null; }
start_linkd() {
  XDG_STATE_HOME=$RUNTIME/state "$BIN/umbriel-linkd" >> "$OUT/linkd.log" 2>&1 &
  LINKD=$!
  for _ in $(seq 100); do prop Visible > /dev/null 2>&1 && return; sleep 0.05; done
  fail "umbriel-linkd did not come up"
}
discover() { in_phone "$BIN/umbriel-quickshare" --name Phone discover --seconds 2; }
send() { in_phone "$BIN/umbriel-quickshare" --name "Test Phone" send "$@" 2>> "$OUT/send.log"; }
wait_signal() { for _ in $(seq 100); do grep -q "$1" "$OUT/signals.txt" && return; sleep 0.1; done; fail "no $1 signal"; }

gdbus monitor --session -d org.umbriel.Link1 -o /org/umbriel/Link1 > "$OUT/signals.txt" 2>&1 &
start_linkd
[[ $(prop Visible) == "(<false>,)" ]] || fail "visible by default: $(prop Visible)"
[[ -z $(discover) ]] || fail "a hidden desktop advertises"
record '{"step":"hidden-by-default"}'

set_visible true
NAME=$(prop Name | sed -E "s/^\(<'(.*)'>,\)$/\1/")
for _ in $(seq 5); do discover | grep -q "\"name\":\"$NAME\"" && break; done
discover | grep -q "\"name\":\"$NAME\"" || fail "visible but not on mDNS as $NAME"
record "{\"step\":\"visible-on-mdns\",\"name\":\"$NAME\"}"

send --to "$NAME" "$RUNTIME/files/photo.jpg" > "$RUNTIME/send1.jsonl" &
SENDER=$!
wait_signal "QuickShare.Offer"
OFFER=$(grep "QuickShare.Offer" "$OUT/signals.txt" | tail -1)
ID=$(echo "$OFFER" | sed -E 's/.*Offer \(uint64 ([0-9]+),.*/\1/')
echo "$OFFER" | grep -q "'Test Phone'" && echo "$OFFER" | grep -q "('photo.jpg', int64 5000000)" || fail "offer lacks sender or file: $OFFER"
[[ -z $(ls -A "$DOWNLOADS") ]] || fail "bytes written before Accept: $(ls -A "$DOWNLOADS")"
qs Accept "$ID"
wait "$SENDER" || fail "the sender failed after Accept"
wait_signal "Finished (uint64 $ID, 'received'"
cmp -s "$RUNTIME/files/photo.jpg" "$DOWNLOADS/photo.jpg" || fail "saved file differs"
PIN=$(sed -n 's/.*"pin":"\([0-9]*\)".*/\1/p' "$RUNTIME/send1.jsonl")
echo "$OFFER" | grep -q "'$PIN'" || fail "PIN on D-Bus differs from the sender's $PIN"
record "{\"step\":\"accept-saves\",\"id\":$ID,\"pin\":\"$PIN\"}"

send --addr 10.79.0.1:4718 "$RUNTIME/files/nope.txt" > "$RUNTIME/send2.jsonl" &
SENDER=$!
for _ in $(seq 100); do [[ $(grep -c "QuickShare.Offer" "$OUT/signals.txt") -ge 2 ]] && break; sleep 0.1; done
ID=$(grep "QuickShare.Offer" "$OUT/signals.txt" | tail -1 | sed -E 's/.*Offer \(uint64 ([0-9]+),.*/\1/')
qs Decline "$ID"
wait "$SENDER" || true
grep -q '"rejected":2' "$RUNTIME/send2.jsonl" || fail "decline not reported to the sender: $(cat "$RUNTIME/send2.jsonl")"
[[ ! -e $DOWNLOADS/nope.txt ]] || fail "a declined file was saved"
wait_signal "Finished (uint64 $ID, 'declined'"
record "{\"step\":\"decline\",\"id\":$ID}"

kill "$LINKD"; wait "$LINKD" || true
start_linkd
[[ $(prop Visible) == "(<true>,)" ]] || fail "visibility lost across a restart"
set_visible false
sleep 1 # real time: the mDNS goodbye
[[ -z $(discover | grep "\"name\":\"$NAME\"") ]] || fail "still advertised after Visible=false"
record '{"step":"persists-and-hides"}'
cat "$OUT/results.jsonl"
echo "PASS; artifacts: $OUT"
