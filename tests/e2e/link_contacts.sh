#!/usr/bin/env bash
# Contacts sync between two network namespaces. Proves: with the phone's messages switch off (as after pairing) the
# phone refuses and D-Bus Contacts stays empty; with it on, 100 contacts arrive in two pages, exactly as the phone holds
# them; an edit on the phone plus its contacts-changed reaches Contacts; the contacts outlive a disconnect and go with
# Unpair. Every message is validated against protocol/link-v1/messages.cddl. Writes results.jsonl, signals.txt,
# transcript.jsonl, hold.jsonl, contacts.json, and linkd.log to $OUT (default ./artifacts/link-contacts).
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
OUT=${OUT:-$(pwd)/artifacts/link-contacts}
BIN=${LINK_BIN:-$ROOT/link/target/debug}
if [[ -z ${LINK_IN_NS:-} ]]; then
  rm -rf "$OUT"
  mkdir -p "$OUT"
  exec env LINK_IN_NS=1 OUT="$OUT" BIN="$BIN" unshare --user --map-root-user --net bash "$0"
fi

RUNTIME=$(mktemp -d /tmp/link-contacts.XXXX)
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


python3 - "$RUNTIME/book.json" <<'PY'
import json, sys
book = [{"name": f"Contact {i:03}", "numbers": [f"+97798{i:08}"] + ([f"01-{i:05}"] if i % 7 == 0 else [])} for i in range(100)]
book[3]["name"] = "Zoë Ünïcode 名前"
json.dump(book, open(sys.argv[1], "w"))
PY
cp "$RUNTIME/book.json" "$OUT/contacts.json"
contacts() { prop Contacts; }
# The D-Bus Contacts value, parsed: device id -> [(name, [numbers])].
matches_book() {
  contacts | python3 -c '
import ast, json, sys
text = sys.stdin.read().strip()
value = ast.literal_eval(text.replace("<", "").replace(">", "").replace("@a{sa(sas)} ", "")[1:-2])
book = [(c["name"], c["numbers"]) for c in json.load(open(sys.argv[2]))]
got = [(name, list(numbers)) for name, numbers in value.get(sys.argv[1], [])]
sys.exit(0 if got == book else 1)' "$ID" "$RUNTIME/book.json"
}

mkfifo "$RUNTIME/phone.in"
hold() {
  exec 3<> "$RUNTIME/phone.in"
  nsenter -t "$PHONE_NS" -n -- "$BIN/umbriel-link-phone" --state "$RUNTIME/phone" --name phone \
    --transcript "$OUT/transcript.jsonl" --contacts "$RUNTIME/book.json" hold < "$RUNTIME/phone.in" >> "$OUT/hold.jsonl" \
    2>> "$OUT/hold.log" &
  HOLD=$!
  wait_for 10 "present phone never shown connected" connected
}
stop_hold() { kill "$HOLD"; wait "$HOLD" 2>/dev/null || true; wait_for 5 "a stopped phone still shows connected" disconnected; }

hold
wait_for 5 "the phone never refused the pull" grep -q "contacts refused: not-allowed" "$OUT/linkd.log"
[[ $(contacts) == "(<@a{sa(sas)} {}>,)" ]] || fail "contacts with the switch off: $(contacts)"
record '{"step":"switch-off-refused","refused":"not-allowed"}'

stop_hold
python3 - "$RUNTIME/phone/devices.json" <<'PY'
import json, sys
store = json.load(open(sys.argv[1]))
store["peers"][0].setdefault("grants", {})["messages"] = True
json.dump(store, open(sys.argv[1], "w"))
PY
hold
wait_for 10 "Contacts never matched the phone's book: $(contacts | head -c 300)" matches_book
# "contacts-list" as a CBOR text string, in the transcript's hex.
PAGES=$(grep -c "6d$(printf contacts-list | xxd -p)" "$OUT/transcript.jsonl" || true)
(( PAGES >= 2 )) || fail "100 contacts came in $PAGES pages"
record "{\"step\":\"pulled\",\"contacts\":100,\"pages\":$PAGES}"

python3 - "$RUNTIME/book.json" <<'PY'
import json, sys
book = json.load(open(sys.argv[1]))
book[10]["numbers"].append("+9779800000999")
del book[50]
json.dump(book, open(sys.argv[1], "w"))
PY
say contacts-changed
wait_for 10 "the edit never reached Contacts" matches_book
record '{"step":"changed-pushed","contacts":99}'

stop_hold
matches_book || fail "contacts did not outlive a disconnect"
record '{"step":"kept-after-disconnect"}'

link Unpair "$ID" > /dev/null
wait_for 5 "contacts outlived Unpair: $(contacts | head -c 200)" eval '[[ $(contacts) == "(<@a{sa(sas)} {}>,)" ]]'
record '{"step":"forgotten-on-unpair"}'

CHECK=$("$BIN/umbriel-link-phone" check-transcript "$ROOT/protocol/link-v1/messages.cddl" "$OUT/transcript.jsonl") \
  || fail "transcript violates the schema"
record "$(echo "$CHECK" | sed 's/^{/{"step":"transcript-schema",/')"
cat "$OUT/results.jsonl"
echo "PASS; artifacts: $OUT"
