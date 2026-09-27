#!/usr/bin/env bash
# Link file transfers across two network namespaces joined by a veth pair, inside an unprivileged user and mount
# namespace. Proves: a 1 GiB file from the phone crosses 5% loss each way and 50 ms of delay, survives the desktop
# daemon being killed mid-transfer (it resumes from the durable offset on disk, re-hashing the partial) and the phone
# moving to a new address mid-transfer, and arrives with a matching SHA-256 after a consent Accept over D-Bus; a file
# from the desktop (D-Bus SendFiles with descriptors) survives the phone being killed mid-transfer and lands in the
# phone's downloads; a declined offer, a cancel mid-transfer (no partial left), and offers larger than the free space of a
# small tmpfs download directory (no-space) and than all of it (too-large) each end as they should; hostile names (`../x`, `.bashrc`, NUL, 600 bytes, `..`) land
# sanitized inside the download directory, and a taken name gets ` (1)`; a hostile phone that streams past the size it
# announced has its stream stopped with protocol-error and nothing published. Every control message and stream header
# is validated against protocol/link-v1/messages.cddl. Writes results.jsonl, throughput.json (throughput and resume
# offsets), hashes.txt, signals.txt, transcript.jsonl, linkd.log, and hold.jsonl to $OUT
# (default ./artifacts/link-files).
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
OUT=${OUT:-$(pwd)/artifacts/link-files}
BIN=${LINK_BIN:-$ROOT/link/target/debug}
if [[ -z ${LINK_IN_NS:-} ]]; then
  rm -rf "$OUT"
  mkdir -p "$OUT"
  exec env LINK_IN_NS=1 OUT="$OUT" BIN="$BIN" unshare --user --map-root-user --net --mount bash "$0"
fi

RUNTIME=$(mktemp -d /tmp/link-files.XXXX)
trap 'kill $(jobs -p) 2>/dev/null || true; wait 2>/dev/null; umount "$RUNTIME/downloads" 2>/dev/null || true; rm -rf "$RUNTIME"' EXIT
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

DOWNLOADS=$RUNTIME/downloads
PHONE_DL=$RUNTIME/phone-downloads
mkdir -p "$DOWNLOADS" "$PHONE_DL" "$RUNTIME/src"
link() { gdbus call --session -d org.umbriel.Link1 -o /org/umbriel/Link1 -m "org.umbriel.Link1.$1" "${@:2}"; }
devices() { gdbus call --session -d org.umbriel.Link1 -o /org/umbriel/Link1 -m org.freedesktop.DBus.Properties.Get org.umbriel.Link1 Devices; }
connected() { devices | grep -q "'phone', true"; }
start_linkd() {
  XDG_STATE_HOME=$RUNTIME/desktop XDG_DOWNLOAD_DIR=$DOWNLOADS "$BIN/umbriel-linkd" >> "$OUT/linkd.log" 2>&1 &
  LINKD=$!
  for _ in $(seq 100); do link CancelPairing > /dev/null 2>&1 && return; sleep 0.05; done
  fail "umbriel-linkd did not come up"
}
port() { python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["port"])' "$RUNTIME/desktop/umbriel-link/devices.json"; }
phone() {
  in_phone "$BIN/umbriel-link-phone" --state "$RUNTIME/phone" --name phone --downloads "$PHONE_DL" \
    --transcript "$OUT/transcript.jsonl" "$@"
}
say() { printf '%s\n' "$*" >&3; }
# The last TransferOffered signal's transfer id.
offered() { grep -A1 'member=TransferOffered' "$OUT/signals.txt" | grep -o '"[0-9a-f]\{32\}"' | tail -1 | tr -d '"'; }
finished() { grep -A2 'member=TransferFinished' "$OUT/signals.txt" | grep -A1 -F "\"$1\"" | grep -o 'string "[a-z-]*"' | tail -1 | cut -d'"' -f2; }
held() { grep "\"transfer\":\"$1\"" "$OUT/hold.jsonl" | grep "\"event\":\"$2\"" | tail -1; }
part_bytes() { stat -c %s "$1"/.*.linkpart 2>/dev/null | sort -n | tail -1 || echo 0; }
sha() { sha256sum "$1" | cut -d' ' -f1; }
netem_on() { in_phone tc qdisc add dev p0 root netem delay 50ms loss 5%; tc qdisc add dev d0 root netem loss 5%; }
netem_off() { in_phone tc qdisc del dev p0 root; tc qdisc del dev d0 root; }

start_linkd
CODE=$(link StartPairing | sed -E "s/^\('([0-9]+)', .*/\1/")
phone pair --code "$CODE" --addr "10.78.0.1:$(port)" > /dev/null || fail "pairing"
ID=$(devices | grep -o "'[0-9a-f]\{32\}', 'phone'" | cut -d"'" -f2)
dbus-monitor --session "type='signal',interface='org.umbriel.Link1'" > "$OUT/signals.txt" 2>&1 &
wait_for 5 "dbus-monitor did not start" test -s "$OUT/signals.txt"

mkfifo "$RUNTIME/phone.in"
exec 3<> "$RUNTIME/phone.in"
start_hold() {
  RUST_LOG=info nsenter -t "$PHONE_NS" -n -- "$BIN/umbriel-link-phone" --state "$RUNTIME/phone" --name phone \
    --downloads "$PHONE_DL" --transcript "$OUT/transcript.jsonl" hold < "$RUNTIME/phone.in" \
    >> "$OUT/hold.jsonl" 2>> "$OUT/hold.log" &
  HOLD=$!
}
# The session starts on the lossy path: quinn's BBR keeps the lowest RTT it ever saw, so delay added under a live
# connection would pin it to its minimum window.
netem_on
start_hold
wait_for 20 "present phone never shown connected" connected

# 1 GiB phone to desktop under loss and delay, through a daemon kill and an address change.
head -c 1073741824 /dev/urandom > "$RUNTIME/src/big.bin"
STARTED=$(date +%s.%N)
say "send $RUNTIME/src/big.bin"
wait_for 60 "no TransferOffered for the big file" eval '[[ -n $(offered) ]]'
BIG=$(offered)
link AcceptTransfer "'$BIG'" > /dev/null || fail "AcceptTransfer"
wait_for 600 "the big file never reached 256 MiB" eval '(( $(part_bytes "$DOWNLOADS") > 268435456 ))'
kill -KILL "$LINKD"; wait "$LINKD" 2> /dev/null || true
KILLED_AT=$(part_bytes "$DOWNLOADS")
start_linkd
wait_for 120 "no resume after the daemon restart" grep -q "transfer $BIG resumes at" "$OUT/linkd.log"
RESUME1=$(grep "transfer $BIG resumes at" "$OUT/linkd.log" | tail -1 | grep -o 'offset: [0-9]*' | cut -d' ' -f2)
record "{\"step\":\"daemon-killed-mid-transfer\",\"part_bytes_at_kill\":$KILLED_AT,\"resumed_at\":$RESUME1}"
wait_for 600 "the big file never reached 640 MiB" eval '(( $(part_bytes "$DOWNLOADS") > 671088640 ))'
in_phone ip addr add 10.78.0.3/24 dev p0
in_phone ip addr del 10.78.0.2/24 dev p0
MOVED_AT=$(part_bytes "$DOWNLOADS")
wait_for 900 "the big file did not finish: $(finished "$BIG")" eval '[[ -n $(finished "$BIG") ]]'
ENDED=$(date +%s.%N)
[[ $(finished "$BIG") == done ]] || fail "big transfer ended $(finished "$BIG")"
RESUMES=$(grep -c "transfer $BIG resumes at" "$OUT/linkd.log")
RESUME2=$( (( RESUMES > 1 )) && grep "transfer $BIG resumes at" "$OUT/linkd.log" | tail -1 | grep -o 'offset: [0-9]*' | cut -d' ' -f2 || echo null)
netem_off
[[ $(sha "$RUNTIME/src/big.bin") == $(sha "$DOWNLOADS/big.bin") ]] || fail "the big file's hash differs"
printf 'big.bin sent %s\nbig.bin received %s\n' "$(sha "$RUNTIME/src/big.bin")" "$(sha "$DOWNLOADS/big.bin")" >> "$OUT/hashes.txt"
ls -A "$DOWNLOADS" | grep -q linkpart && fail "a part file was left behind"
record "{\"step\":\"address-change-mid-transfer\",\"part_bytes_at_move\":$MOVED_AT,\"migrated\":$( (( RESUMES == 1 )) && echo true || echo false),\"resumed_at\":$RESUME2}"
python3 - "$OUT/throughput.json" 1073741824 "$STARTED" "$ENDED" "$KILLED_AT" "$RESUME1" "$MOVED_AT" "$RESUME2" <<'PY'
import json, sys
out, size, start, end, killed, resume1, moved, resume2 = sys.argv[1:]
seconds = float(end) - float(start)
json.dump({
    "file_bytes": int(size), "seconds": round(seconds, 1), "mib_per_s": round(int(size) / 2**20 / seconds, 2),
    "netem": {"phone_egress": "delay 50ms loss 5%", "desktop_egress": "loss 5%"},
    "daemon_kill": {"part_bytes": int(killed), "resumed_at": int(resume1)},
    "address_change": {"part_bytes": int(moved), "resumed_at": None if resume2 == "null" else int(resume2)},
}, open(out, "w"), indent=2)
PY
record "{\"step\":\"big-file-under-loss\",\"sha256_match\":true}"
cat "$OUT/throughput.json"
rm -f "$DOWNLOADS/big.bin"

# Desktop to phone, through a phone kill.
head -c 536870912 /dev/urandom > "$RUNTIME/src/down.bin"
DOWN=$(python3 "$ROOT/tests/e2e/link_send_files.py" "$ID" "$RUNTIME/src/down.bin") || fail "SendFiles"
wait_for 20 "the phone never saw the offer" eval '[[ -n $(held "$DOWN" offered) ]]'
say "accept $DOWN"
wait_for 120 "the phone's part never reached 64 MiB" eval '(( $(part_bytes "$PHONE_DL") > 67108864 ))'
kill -KILL "$HOLD"; wait "$HOLD" 2> /dev/null || true
PHONE_KILLED_AT=$(part_bytes "$PHONE_DL")
start_hold
wait_for 120 "the desktop-to-phone transfer did not finish" eval '[[ -n $(finished "$DOWN") ]]'
[[ $(finished "$DOWN") == done ]] || fail "desktop-to-phone ended $(finished "$DOWN")"
wait_for 10 "the phone did not report its file" eval '[[ -n $(held "$DOWN" finished) ]]'
held "$DOWN" finished | grep -q '"status":"done"' || fail "the phone's result: $(held "$DOWN" finished)"
[[ $(sha "$RUNTIME/src/down.bin") == $(sha "$PHONE_DL/down.bin") ]] || fail "the phone's copy differs"
printf 'down.bin sent %s\ndown.bin received %s\n' "$(sha "$RUNTIME/src/down.bin")" "$(sha "$PHONE_DL/down.bin")" >> "$OUT/hashes.txt"
record "{\"step\":\"desktop-to-phone-through-phone-kill\",\"part_bytes_at_kill\":$PHONE_KILLED_AT,\"sha256_match\":true}"

# Decline, and cancel mid-transfer.
head -c 4096 /dev/urandom > "$RUNTIME/src/small.bin"
say "send $RUNTIME/src/small.bin"
wait_for 20 "no offer for the small file" eval '[[ $(offered) != "$BIG" && -n $(offered) ]]'
SMALL=$(offered)
link DeclineTransfer "'$SMALL'" > /dev/null || fail "DeclineTransfer"
wait_for 10 "the phone did not see the decline" eval '[[ -n $(held "$SMALL" finished) ]]'
held "$SMALL" finished | grep -q '"status":"declined"' || fail "declined offer: $(held "$SMALL" finished)"
[[ -z $(ls -A "$DOWNLOADS") ]] || fail "a declined offer left files: $(ls -A "$DOWNLOADS")"
record '{"step":"declined","phone_status":"declined"}'

head -c 536870912 /dev/urandom > "$RUNTIME/src/cancel.bin"
say "send $RUNTIME/src/cancel.bin"
wait_for 60 "no offer for the file to cancel" eval '[[ $(offered) != "$SMALL" ]]'
CANCEL=$(offered)
link AcceptTransfer "'$CANCEL'" > /dev/null
wait_for 60 "the file to cancel never started" eval '(( $(part_bytes "$DOWNLOADS") > 16777216 ))'
link CancelTransfer "'$CANCEL'" > /dev/null || fail "CancelTransfer"
wait_for 10 "the phone did not see the cancel" eval '[[ -n $(held "$CANCEL" finished) ]]'
held "$CANCEL" finished | grep -q '"status":"cancelled"' || fail "cancel: $(held "$CANCEL" finished)"
[[ $(finished "$CANCEL") == cancelled ]] || fail "desktop cancel: $(finished "$CANCEL")"
[[ -z $(ls -A "$DOWNLOADS") ]] || fail "a cancelled transfer left files: $(ls -A "$DOWNLOADS")"
record '{"step":"cancelled-mid-transfer","partials_left":0}'

mount -t tmpfs -o size=8m tmpfs "$DOWNLOADS"
head -c 6291456 /dev/zero > "$DOWNLOADS/filler"
head -c 4194304 /dev/urandom > "$RUNTIME/src/four.bin"
head -c 16777216 /dev/urandom > "$RUNTIME/src/sixteen.bin"
say "send $RUNTIME/src/four.bin"
wait_for 20 "the no-space offer did not end" eval 'grep -q "\"status\":\"no-space\"" "$OUT/hold.jsonl"'
say "send $RUNTIME/src/sixteen.bin"
wait_for 20 "the too-large offer did not end" eval 'grep -q "\"status\":\"too-large\"" "$OUT/hold.jsonl"'
[[ $(ls -A "$DOWNLOADS") == filler ]] || fail "a refused offer left files: $(ls -A "$DOWNLOADS")"
umount "$DOWNLOADS"
record '{"step":"refused-for-space","downloads":"8 MiB tmpfs, 6 MiB used","no_space_bytes":4194304,"too_large_bytes":16777216}'

kill "$HOLD"; wait "$HOLD" 2> /dev/null || true
link SetAutoAccept "'$ID'" true > /dev/null || fail "SetAutoAccept"
printf 'hostile' > "$RUNTIME/src/h.txt"
phone send-file "$RUNTIME/src/h.txt" "$RUNTIME/src/h.txt" "$RUNTIME/src/h.txt" "$RUNTIME/src/h.txt" "$RUNTIME/src/h.txt" \
  "$RUNTIME/src/h.txt" --as-name '../x' --as-name '.bashrc' --as-name 'a%00b.txt' \
  --as-name "$(printf 'é%.0s' $(seq 300)).txt" --as-name '..' --as-name 'report.txt' > "$OUT/hostile-names.jsonl" \
  || fail "sending hostile names"
phone send-file "$RUNTIME/src/h.txt" --as-name 'report.txt' >> "$OUT/hostile-names.jsonl" || fail "second report.txt"
LONG=$(printf 'é%.0s' $(seq 127))
for name in x bashrc ab.txt "$LONG" file report.txt "report (1).txt"; do
  [[ -f $DOWNLOADS/$name ]] || fail "no sanitized file \"$name\": $(ls -A "$DOWNLOADS")"
done
[[ $(ls -A "$DOWNLOADS" | wc -l) == 7 ]] || fail "unexpected files: $(ls -A "$DOWNLOADS")"
[[ ! -e $RUNTIME/x ]] || fail "a name escaped the download directory"
record "{\"step\":\"hostile-names\",\"published\":$(ls -A "$DOWNLOADS" | python3 -c 'import json,sys; print(json.dumps(sorted(l.rstrip("\n") for l in sys.stdin)))')}"

printf 'twelve bytes' > "$RUNTIME/src/over.txt"
OUTPUT=$(phone send-file "$RUNTIME/src/over.txt" --oversize 16777216) || fail "oversize sender: $OUTPUT"
echo "$OUTPUT" | grep -q '"stopped_with":5' || fail "the oversized stream was not stopped with protocol-error: $OUTPUT"
echo "$OUTPUT" | grep -q '"ok":false' || fail "the oversized file was not failed: $OUTPUT"
[[ ! -e $DOWNLOADS/over.txt ]] || fail "an oversized file was published"
ls -A "$DOWNLOADS" | grep -q linkpart && fail "an oversized file left its part"
record "$(echo "$OUTPUT" | sed 's/^{/{"step":"oversize-stopped",/')"

CHECK=$("$BIN/umbriel-link-phone" check-transcript "$ROOT/protocol/link-v1/messages.cddl" "$OUT/transcript.jsonl") \
  || fail "transcript violates the schema"
for kind in offer offer-reply resume resume-at cancel file-done file-data; do
  python3 - "$OUT/transcript.jsonl" "$kind" <<'PY' || fail "the transcript has no $kind"
import json, sys
kind = sys.argv[2].encode()
if not any(kind in bytes.fromhex(json.loads(line)["cbor"]) for line in open(sys.argv[1])):
    sys.exit(1)
PY
done
record "$(echo "$CHECK" | sed 's/^{/{"step":"transcript-schema",/')"
cat "$OUT/results.jsonl"
echo "PASS; artifacts: $OUT"
