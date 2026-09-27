#!/usr/bin/env bash
# Quick Share over the LAN between two network namespaces joined by a veth pair, inside an unprivileged user namespace:
# our receiver on the desktop side, our sender on the phone side (stock Android was checked by hand against the same
# receiver). Proves: the sender finds the receiver by mDNS alone; both ends derive the same PIN; two files arrive
# byte-identical, the second of the same name as "name (1)"; 5% loss and 30 ms delay still deliver an intact 20 MB
# file; hostile names ("../../evil", ".bashrc") land as bare names in the target directory; bytes past the announced
# size fail the transfer and leave nothing behind; a declined offer writes nothing. Writes results.jsonl,
# receive.jsonl, decline.jsonl, and hashes.txt to $OUT (default ./artifacts/quickshare).
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
OUT=${OUT:-$(pwd)/artifacts/quickshare}
BIN=${LINK_BIN:-$ROOT/link/target/debug}
if [[ -z ${QS_IN_NS:-} ]]; then
  rm -rf "$OUT"
  mkdir -p "$OUT"
  exec env QS_IN_NS=1 OUT="$OUT" BIN="$BIN" unshare --user --map-root-user --net bash "$0"
fi

RUNTIME=$(mktemp -d /tmp/qs-e2e.XXXX)
trap 'kill $(jobs -p) 2>/dev/null || true; wait 2>/dev/null; rm -rf "$RUNTIME"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }
record() { printf '%s\n' "$1" >> "$OUT/results.jsonl"; }

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

QS=$BIN/umbriel-quickshare
IN=$RUNTIME/in
mkdir -p "$IN" "$RUNTIME/declined" "$RUNTIME/files"
head -c 20000000 /dev/urandom > "$RUNTIME/files/blob.bin"
echo "a small note" > "$RUNTIME/files/note.txt"
"$QS" --name Desktop receive --dir "$IN" --port 47100 --consent accept > "$OUT/receive.jsonl" 2> "$OUT/receive.log" &
"$QS" --name Decliner receive --dir "$RUNTIME/declined" --port 47101 --consent decline > "$OUT/decline.jsonl" \
  2> "$OUT/decline.log" &
for _ in $(seq 50); do [[ $(grep -c listening "$OUT/receive.jsonl" "$OUT/decline.jsonl" | grep -c ':1') == 2 ]] && break; sleep 0.1; done

send() { in_phone "$QS" --name Phone send "$@" 2>> "$OUT/send.log"; }
last_received() { grep '"received"\|"failed"' "$OUT/receive.jsonl" | tail -1; }
wait_results() { for _ in $(seq 100); do [[ $(grep -c '"received"\|"failed"' "$OUT/receive.jsonl") -ge $1 ]] && return; sleep 0.1; done; fail "receiver reported nothing"; }

FOUND=$(in_phone "$QS" --name Phone discover --seconds 2)
echo "$FOUND" | grep '"name":"Desktop"' | grep -q '10.78.0.1:47100' || fail "mDNS did not find the receiver: $FOUND"
record "$(echo "$FOUND" | grep Desktop | sed 's/^{/{"step":"mdns-discovery",/')"

OUTPUT=$(send --to Desktop "$RUNTIME/files/blob.bin" "$RUNTIME/files/note.txt") || fail "send failed"
wait_results 1
PHONE_PIN=$(echo "$OUTPUT" | sed -n 's/.*"pin":"\([0-9]*\)".*/\1/p')
DESKTOP_PIN=$(grep '"offer"' "$OUT/receive.jsonl" | tail -1 | sed -n 's/.*"pin":"\([0-9]*\)".*/\1/p')
[[ -n $PHONE_PIN && $PHONE_PIN == "$DESKTOP_PIN" ]] || fail "PINs differ: phone $PHONE_PIN desktop $DESKTOP_PIN"
cmp -s "$RUNTIME/files/blob.bin" "$IN/blob.bin" && cmp -s "$RUNTIME/files/note.txt" "$IN/note.txt" || fail "files differ"
record "{\"step\":\"two-files\",\"pin\":\"$PHONE_PIN\"}"

send --addr 10.78.0.1:47100 "$RUNTIME/files/note.txt" > /dev/null || fail "second send failed"
wait_results 2
cmp -s "$RUNTIME/files/note.txt" "$IN/note (1).txt" || fail "a repeated name did not become 'note (1).txt': $(ls "$IN")"
record '{"step":"name-collision","saved_as":"note (1).txt"}'

in_phone tc qdisc add dev p0 root netem loss 5% delay 30ms
START=$(date +%s.%N)
send --addr 10.78.0.1:47100 --offer-as lossy.bin "$RUNTIME/files/blob.bin" > /dev/null || fail "send under loss failed"
SECONDS_TAKEN=$(awk -v s="$START" -v e="$(date +%s.%N)" "BEGIN {printf \"%.2f\", e - s}")
in_phone tc qdisc del dev p0 root
wait_results 3
cmp -s "$RUNTIME/files/blob.bin" "$IN/lossy.bin" || fail "file sent under loss differs"
record "{\"step\":\"loss-5pct-delay-30ms\",\"bytes\":20000000,\"seconds\":$SECONDS_TAKEN}"

send --addr 10.78.0.1:47100 --offer-as "../../evil" "$RUNTIME/files/note.txt" > /dev/null || fail "hostile send failed"
send --addr 10.78.0.1:47100 --offer-as ".bashrc" "$RUNTIME/files/note.txt" > /dev/null || fail "hostile send failed"
wait_results 5
[[ -f $IN/evil && -f $IN/bashrc && ! -e $RUNTIME/evil && ! -e $IN/.bashrc ]] || fail "hostile names escaped: $(ls -a "$IN")"
record '{"step":"hostile-names","saved_as":["evil","bashrc"]}'

send --addr 10.78.0.1:47100 --offer-as oversize.txt --extra-bytes 4096 "$RUNTIME/files/note.txt" > /dev/null || true
wait_results 6
last_received | grep -q '"error":"protocol violation: file longer than announced"' || fail "oversize not refused: $(last_received)"
[[ -z $(find "$IN" -name '*oversize*') ]] || fail "oversize left a file behind: $(ls -a "$IN")"
record '{"step":"oversize-refused","left_behind":0}'

OUTPUT=$(send --addr 10.78.0.1:47101 "$RUNTIME/files/note.txt") || fail "send to the decliner failed"
echo "$OUTPUT" | grep -q '"rejected":2' || fail "decline not reported to the sender: $OUTPUT"
[[ -z $(ls -A "$RUNTIME/declined") ]] || fail "a declined offer wrote files"
record '{"step":"declined","written":0}'

[[ -z $(find "$IN" -name '*.qspart') ]] || fail "part files left behind"
sha256sum "$RUNTIME/files/blob.bin" "$IN/blob.bin" "$IN/lossy.bin" | sed "s|$RUNTIME/||" > "$OUT/hashes.txt"
cat "$OUT/results.jsonl"
echo "PASS; artifacts: $OUT"
