#!/usr/bin/env bash
# Link phase 0 across two network namespaces (desktop and phone) joined by a veth pair, inside an unprivileged user
# namespace. Proves: an unpaired desktop is silent on mDNS; code pairing through mDNS; a relay terminating TLS on both
# legs fails even with the right code; a wrong code fails and burns the window; with multicast dropped on the phone
# side the phone still reconnects through its last-known address after a daemon restart, and the second reconnect
# resumes the TLS session; QR pairing works without multicast; D-Bus shows live sessions; unpairing from either side
# is honoured. Every control message the phone saw is validated against protocol/link-v1/messages.cddl.
# Writes results.jsonl, transcript.jsonl, relay.jsonl, linkd.log to $OUT (default ./artifacts/link-pair).
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
OUT=${OUT:-$(pwd)/artifacts/link-pair}
BIN=${LINK_BIN:-$ROOT/link/target/debug}
if [[ -z ${LINK_IN_NS:-} ]]; then
  rm -rf "$OUT"
  mkdir -p "$OUT"
  exec env LINK_IN_NS=1 OUT="$OUT" BIN="$BIN" unshare --user --map-root-user --net bash "$0"
fi

RUNTIME=$(mktemp -d /tmp/link-e2e.XXXX)
trap 'kill $(jobs -p) 2>/dev/null || true; wait 2>/dev/null; rm -rf "$RUNTIME"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }
record() { printf '%s\n' "$1" >> "$OUT/results.jsonl"; }

ip link set lo up
ip link add d0 type veth peer name p0
unshare --net sleep infinity &
PHONE_NS=$!
for _ in $(seq 50); do [[ $(readlink /proc/$PHONE_NS/ns/net) != "$(readlink /proc/self/ns/net)" ]] && break; sleep 0.02; done
ip link set p0 netns "$PHONE_NS"
ip addr add 10.77.0.1/24 dev d0
ip link set d0 up
in_phone() { nsenter -t "$PHONE_NS" -n -- "$@"; }
in_phone ip link set lo up
in_phone ip addr add 10.77.0.2/24 dev p0
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
start_linkd() {
  XDG_STATE_HOME=$RUNTIME/desktop "$BIN/umbriel-linkd" >> "$OUT/linkd.log" 2>&1 &
  LINKD=$!
  for _ in $(seq 100); do link CancelPairing > /dev/null 2>&1 && return; sleep 0.05; done
  fail "umbriel-linkd did not come up"
}
pairing() { link StartPairing | sed -E "s/^\('([0-9]+)', '([^']+)'\)$/\\$1/"; }
port() { python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["port"])' "$RUNTIME/desktop/umbriel-link/devices.json"; }
phone() {
  local state=$1; shift
  in_phone "$BIN/umbriel-link-phone" --state "$RUNTIME/$state" --name "$state" --transcript "$OUT/transcript.jsonl" "$@"
}
count_devices() { devices | grep -o "'[0-9a-f]\{32\}'" | wc -l; }

start_linkd
PORT=$(port)
[[ $(in_phone "$BIN/umbriel-link-phone" discover --seconds 2 | wc -l) == 0 ]] || fail "an unpaired desktop advertises"
record '{"step":"silent-when-unpaired"}'

CODE=$(pairing 1)
OUTPUT=$(phone phone1 pair --code "$CODE") || fail "code pairing over mDNS"
record "$(echo "$OUTPUT" | sed 's/^{/{"step":"code-pair-mdns",/')"
[[ $(count_devices) == 1 ]] || fail "desktop does not list phone1: $(devices)"

CODE=$(pairing 1)
in_phone "$BIN/umbriel-link-phone" relay --listen 10.77.0.2:40000 --target "10.77.0.1:$PORT" > "$OUT/relay.jsonl" 2>&1 &
RELAY=$!
for _ in $(seq 50); do grep -q relay "$OUT/relay.jsonl" && break; sleep 0.05; done
if phone phone2 pair --code "$CODE" --addr 10.77.0.2:40000 2> "$RUNTIME/relay-pair.err"; then fail "relayed pairing succeeded"; fi
wait "$RELAY" || true
[[ $(count_devices) == 1 ]] || fail "relayed pairing stored a key"
grep -q "pairing confirmation does not match" "$OUT/linkd.log" || fail "desktop did not reject the relay's confirmation"
record "{\"step\":\"relay-rejected\",\"phone_error\":\"$(tr -d '"\n' < "$RUNTIME/relay-pair.err")\"}"

CODE=$(pairing 1)
WRONG=$(printf '%06d' $(( (10#$CODE + 1) % 1000000 )))
if phone phone2 pair --code "$WRONG" --addr "10.77.0.1:$PORT" 2> /dev/null; then fail "wrong code paired"; fi
if phone phone2 pair --code "$CODE" --addr "10.77.0.1:$PORT" 2> /dev/null; then fail "window survived a failed attempt"; fi
[[ $(count_devices) == 1 ]] || fail "failed attempts stored a key"
record '{"step":"wrong-code-rejected","window_burned":true}'

in_phone nft -f - <<'NFT'
table inet no_mdns {
  chain out { type filter hook output priority 0; udp dport 5353 drop; }
  chain in { type filter hook input priority 0; udp sport 5353 drop; }
}
NFT
FOUND=$(in_phone "$BIN/umbriel-link-phone" discover --seconds 2 | wc -l)
[[ $FOUND == 0 ]] || fail "mDNS still works with multicast dropped"

kill "$LINKD"; wait "$LINKD" || true
start_linkd
[[ $(port) == "$PORT" ]] || fail "daemon changed port across a restart"
OUTPUT=$(phone phone1 connect --times 2) || fail "reconnect without multicast"
echo "$OUTPUT" | sed 's/^{/{"step":"reconnect-no-multicast",/' >> "$OUT/results.jsonl"
[[ $(echo "$OUTPUT" | grep -c '"via":"last-known"') == 2 ]] || fail "reconnect did not use last-known addresses: $OUTPUT"
echo "$OUTPUT" | tail -1 | grep -q '"resumed":true' || fail "second reconnect did not resume the TLS session: $OUTPUT"

URI=$(pairing 2)
OUTPUT=$(phone phone3 pair --uri "$URI") || fail "QR pairing without multicast"
record "$(echo "$OUTPUT" | sed 's/^{/{"step":"qr-pair-no-multicast",/')"
[[ $(count_devices) == 2 ]] || fail "desktop does not list phone3"

phone phone1 connect --hold 3 > /dev/null &
HOLD=$!
for _ in $(seq 40); do devices | grep -q "'phone1', true" && break; sleep 0.05; done
devices | grep -q "'phone1', true" || fail "live session not shown on D-Bus: $(devices)"
wait "$HOLD"
record '{"step":"session-visible-on-dbus"}'

PHONE3_ID=$(devices | grep -o "'[0-9a-f]\{32\}', 'phone3'" | cut -d"'" -f2)
link Unpair "$PHONE3_ID" > /dev/null
OUTPUT=$(phone phone3 connect) || fail "unpaired phone errored instead of forgetting"
echo "$OUTPUT" | grep -q '"forgotten"' || fail "phone3 did not forget the desktop: $OUTPUT"
record "$(echo "$OUTPUT" | sed 's/^{/{"step":"desktop-unpair-honoured",/')"

OUTPUT=$(phone phone1 unpair) || fail "phone-side unpair"
record "$(echo "$OUTPUT" | sed 's/^{/{"step":"phone-unpair",/')"
for _ in $(seq 40); do [[ $(count_devices) == 0 ]] && break; sleep 0.05; done
[[ $(count_devices) == 0 ]] || fail "desktop kept phone1 after it unpaired: $(devices)"

CHECK=$("$BIN/umbriel-link-phone" check-transcript "$ROOT/protocol/link-v1/messages.cddl" "$OUT/transcript.jsonl") \
  || fail "transcript violates the schema"
record "$(echo "$CHECK" | sed 's/^{/{"step":"transcript-schema",/')"
cat "$OUT/results.jsonl"
echo "PASS; artifacts: $OUT"
