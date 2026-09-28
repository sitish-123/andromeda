#!/usr/bin/env bash
# Link across an overlay network (Tailscale) without Tailscale: a second veth pair named tailscale0 on the desktop,
# 100.101.0.0/24 (inside Tailscale's 100.64.0.0/10), beside the LAN pair 10.81.0.0/24, plus container and VM bridges
# (docker0, virbr0) as a desktop with Docker has.
#
# Proves:
# 1. The desktop's hello lists its LAN address, then its overlay address, and never a bridge's.
# 2. A phone whose store already holds 8 stale addresses (a full list) still takes the new ones on connect.
# 3. With the LAN gone, the phone reaches the desktop at its overlay address (via "last-known").
# Writes results.jsonl, phone.json, linkd.log to $OUT (default ./artifacts/link-tailnet).
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
OUT=${OUT:-$(pwd)/artifacts/link-tailnet}
BIN=${LINK_BIN:-$ROOT/link/target/debug}
if [[ -z ${LINK_IN_NS:-} ]]; then
  rm -rf "$OUT"
  mkdir -p "$OUT"
  exec env LINK_IN_NS=1 OUT="$OUT" BIN="$BIN" unshare --user --map-root-user --net bash "$0"
fi

RUNTIME=$(mktemp -d /tmp/link-tailnet.XXXX)
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

ip link add tailscale0 type veth peer name tp0
ip link set tp0 netns "$PHONE_NS"
ip addr add 100.101.0.1/24 dev tailscale0
ip link set tailscale0 up
in_phone ip addr add 100.101.0.2/24 dev tp0
in_phone ip link set tp0 up
for bridge in docker0 virbr0; do ip link add "$bridge" type dummy; ip link set "$bridge" up; done
ip addr add 172.17.0.1/16 dev docker0
ip addr add 192.168.122.1/24 dev virbr0

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
XDG_STATE_HOME=$RUNTIME/desktop RUST_LOG=info "$BIN/umbriel-linkd" >> "$OUT/linkd.log" 2>&1 &
for _ in $(seq 100); do link CancelPairing > /dev/null 2>&1 && break; sleep 0.05; done
link CancelPairing > /dev/null || fail "umbriel-linkd did not come up"
PORT=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["port"])' "$RUNTIME/desktop/umbriel-link/devices.json")
phone() { in_phone "$BIN/umbriel-link-phone" --state "$RUNTIME/phone" --name phone "$@" 2>> "$OUT/phone.log"; }
addresses() { python3 -c 'import json,sys; print(" ".join(json.load(open(sys.argv[1]))["peers"][0]["addresses"]))' "$RUNTIME/phone/devices.json"; }

CODE=$(link StartPairing | sed -E "s/^\('([0-9]+)', .*/\1/")
phone pair --code "$CODE" --addr "10.81.0.1:$PORT" > /dev/null || fail "pairing"
KNOWN=$(addresses)
[[ $KNOWN == *"100.101.0.1:$PORT"* ]] || fail "the hello did not list the overlay address: $KNOWN"
[[ $KNOWN != *172.17.* && $KNOWN != *192.168.122.* ]] || fail "the hello listed a bridge: $KNOWN"
python3 - "$KNOWN" "$PORT" <<'PY' || fail "overlay address not right after the LAN one: $KNOWN"
import sys
known, port = sys.argv[1].split(), sys.argv[2]
v4 = [a for a in known if not a.startswith("[")]
assert v4.index(f"10.81.0.1:{port}") < v4.index(f"100.101.0.1:{port}"), known
PY
record "{\"step\":\"hello-lists-overlay-not-bridges\",\"addresses\":\"$KNOWN\"}"

python3 - "$RUNTIME/phone/devices.json" "$PORT" <<'PY'
import json, sys
path, port = sys.argv[1], sys.argv[2]
store = json.load(open(path))
store["peers"][0]["addresses"] = [f"10.81.0.1:{port}"] + [f"10.99.0.{n}:{port}" for n in range(1, 8)]
json.dump(store, open(path, "w"))
PY
phone connect > "$OUT/phone.json" || fail "connecting over the LAN with a full list"
[[ $(addresses) == *"100.101.0.1:$PORT"* ]] || fail "a full list did not take the overlay address: $(addresses)"
record '{"step":"full-list-takes-new-addresses"}'

ip link set d0 down
phone connect >> "$OUT/phone.json" || fail "the phone could not reach the desktop without the LAN"
LINE=$(tail -1 "$OUT/phone.json")
[[ $LINE == *"\"addr\":\"100.101.0.1:$PORT\""* ]] || fail "not reached at the overlay address: $LINE"
[[ $LINE == *'"via":"last-known"'* ]] || fail "not reached via a last-known address: $LINE"
record '{"step":"reached-over-overlay-without-lan","via":"last-known"}'
cat "$OUT/results.jsonl"
echo "PASS; artifacts: $OUT"
