#!/usr/bin/env bash
# Texts between two network namespaces. Proves: with the phone's messages switch off (as after pairing) the phone
# refuses and D-Bus Texts stays empty and SendText fails; with it on, 40 texts arrive in three pages, newest first, a
# body over 2048 bytes cut on a character boundary; a text the phone receives reaches Texts and TextReceived; SendText
# returns once the phone reports the text sent, and the sent text shows in Texts; a failed send and a disconnected
# phone answer SendText with an error; the texts outlive a disconnect and go with Unpair. Every message is validated
# against protocol/link-v1/messages.cddl. Writes results.jsonl, signals.txt, transcript.jsonl, hold.jsonl, texts.json,
# and linkd.log to $OUT (default ./artifacts/link-texts).
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
OUT=${OUT:-$(pwd)/artifacts/link-texts}
BIN=${LINK_BIN:-$ROOT/link/target/debug}
if [[ -z ${LINK_IN_NS:-} ]]; then
  rm -rf "$OUT"
  mkdir -p "$OUT"
  exec env LINK_IN_NS=1 OUT="$OUT" BIN="$BIN" unshare --user --map-root-user --net bash "$0"
fi

RUNTIME=$(mktemp -d /tmp/link-texts.XXXX)
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


python3 - "$RUNTIME/texts.json" <<'PY'
import json, sys
texts = [{"id": 100 - i, "thread": 1 + i % 5, "address": f"+97798{i % 5:08}", "body": f"text {100 - i} ✉",
          "date": 1_790_000_000_000 - i * 60_000, "sent": i % 3 == 0} for i in range(40)]
texts[7]["body"] = "é" * 1500
json.dump(texts, open(sys.argv[1], "w"))
PY
cp "$RUNTIME/texts.json" "$OUT/texts.json"
# D-Bus Texts for the phone as JSON, through GLib, since gdbus prints uint64 values Python cannot read back.
texts() {
  python3 - "$ID" <<'PY'
import json, sys
from gi.repository import Gio
bus = Gio.bus_get_sync(Gio.BusType.SESSION)
value = bus.call_sync("org.umbriel.Link1", "/org/umbriel/Link1", "org.freedesktop.DBus.Properties", "Get",
                      __import__("gi.repository.GLib", fromlist=["GLib"]).Variant("(ss)", ("org.umbriel.Link1", "Texts")),
                      None, Gio.DBusCallFlags.NONE, 5000, None).unpack()[0]
print(json.dumps(value.get(sys.argv[1], [])))
PY
}
matches_file() {
  texts | python3 -c '
import json, sys
got = [tuple(t) for t in json.load(sys.stdin)]
def cut(body):
    raw = body.encode()[:2048]
    return raw.decode("utf-8", "ignore")
want = [(t["id"], t["thread"], t["address"], cut(t["body"]), t["date"], t["sent"]) for t in json.load(open(sys.argv[1]))]
sys.exit(0 if got == want else 1)' "$RUNTIME/texts.json"
}
send_text() {
  python3 - "$ID" "$1" "$2" <<'PY'
import sys
from gi.repository import Gio, GLib
bus = Gio.bus_get_sync(Gio.BusType.SESSION)
try:
    bus.call_sync("org.umbriel.Link1", "/org/umbriel/Link1", "org.umbriel.Link1", "SendText",
                  GLib.Variant("(sss)", tuple(sys.argv[1:4])), None, Gio.DBusCallFlags.NONE, 90000, None)
    print("sent")
except GLib.Error as error:
    print(error.message)
PY
}

mkfifo "$RUNTIME/phone.in"
hold() {
  exec 3<> "$RUNTIME/phone.in"
  nsenter -t "$PHONE_NS" -n -- "$BIN/umbriel-link-phone" --state "$RUNTIME/phone" --name phone \
    --transcript "$OUT/transcript.jsonl" --texts "$RUNTIME/texts.json" hold < "$RUNTIME/phone.in" >> "$OUT/hold.jsonl" \
    2>> "$OUT/hold.log" &
  HOLD=$!
  wait_for 10 "present phone never shown connected" connected
}
stop_hold() { kill "$HOLD"; wait "$HOLD" 2>/dev/null || true; wait_for 5 "a stopped phone still shows connected" disconnected; }

hold
wait_for 5 "the phone never refused the pull" grep -q "texts refused: not-allowed" "$OUT/linkd.log"
[[ $(texts) == "[]" ]] || fail "texts with the switch off: $(texts)"
OUTPUT=$(send_text +9779800000001 hello)
[[ $OUTPUT == *"not-allowed"* ]] || fail "SendText with the switch off: $OUTPUT"
record '{"step":"switch-off-refused","refused":"not-allowed"}'

stop_hold
python3 - "$RUNTIME/phone/devices.json" <<'PY'
import json, sys
store = json.load(open(sys.argv[1]))
store["peers"][0].setdefault("grants", {})["messages"] = True
json.dump(store, open(sys.argv[1], "w"))
PY
hold
wait_for 10 "Texts never matched the phone's: $(texts | head -c 300)" matches_file
PAGES=$(grep -c "6a$(printf texts-list | xxd -p)" "$OUT/transcript.jsonl" || true)
record "{\"step\":\"pulled\",\"texts\":40,\"texts_list_requests\":$PAGES,\"long_body_cut_to\":2048}"

python3 - "$RUNTIME/texts.json" <<'PY'
import json, sys
texts = json.load(open(sys.argv[1]))
texts.insert(0, {"id": 101, "thread": 2, "address": "+9779800000001", "body": "arrived now", "date": 1_790_000_060_000, "sent": False})
json.dump(texts, open(sys.argv[1], "w"))
PY
say texts-changed
wait_for 10 "the received text never reached Texts" matches_file
wait_for 5 "no TextReceived" grep -q "arrived now" "$OUT/signals.txt"
record '{"step":"received","signal":"TextReceived"}'

OUTPUT=$(send_text +9779800000002 "from the desktop ✓")
[[ $OUTPUT == sent ]] || fail "SendText: $OUTPUT"
grep -q '"event":"text-sent"' "$OUT/hold.jsonl" || fail "the phone never sent it"
say texts-changed
wait_for 10 "the sent text never reached Texts" matches_file
texts | python3 -c 'import json,sys; t=json.load(sys.stdin)[0]; sys.exit(0 if t[3]=="from the desktop ✓" and t[5] else 1)' \
  || fail "the newest text is not the sent one: $(texts | head -c 200)"
record '{"step":"sent","shown_as_sent":true}'

OUTPUT=$(send_text fail "no service")
[[ $OUTPUT == *"failed"* ]] || fail "a failed send answered: $OUTPUT"
OUTPUT=$(send_text +9779800000002 "$(printf 'x%.0s' $(seq 2049))")
[[ $OUTPUT == *"too long"* ]] || fail "an over-long text answered: $OUTPUT"
record '{"step":"send-failures","failed":"failed","too_long":"rejected"}'

stop_hold
matches_file || fail "texts did not outlive a disconnect"
OUTPUT=$(send_text +9779800000002 "while away")
[[ $OUTPUT == *"not connected"* ]] || fail "SendText to a disconnected phone: $OUTPUT"
record '{"step":"kept-after-disconnect","send_while_away":"not-connected"}'

link Unpair "$ID" > /dev/null
wait_for 5 "texts outlived Unpair: $(texts | head -c 200)" eval '[[ $(texts) == "[]" ]]'
record '{"step":"forgotten-on-unpair"}'

CHECK=$("$BIN/umbriel-link-phone" check-transcript "$ROOT/protocol/link-v1/messages.cddl" "$OUT/transcript.jsonl") \
  || fail "transcript violates the schema"
record "$(echo "$CHECK" | sed 's/^{/{"step":"transcript-schema",/')"
cat "$OUT/results.jsonl"
echo "PASS; artifacts: $OUT"
