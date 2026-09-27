#!/usr/bin/env bash
# umbriel-linkd's LocalSend v2 backend across two network namespaces, against LocalSend peers written from the
# protocol spec: curl for the sending side, and tests/e2e/localsend_peer.py (Python standard library) for discovery and
# the receiving side. Proves: the backend is off until SetLocalSendVisible (nothing listens on 53317); once on, the
# HTTPS certificate's SHA-256 is the fingerprint /info reports and the one multicast announcements carry; a peer's
# announcement is answered with a register POST and lists it in Nearby; prepare-upload becomes TransferOffered and
# waits for consent: accepted, the upload lands in Downloads with the sender's SHA-256; declined, prepare-upload gets
# 403; a second prepare-upload while one is open gets 409; hostile names (`../evil`, `.bashrc`) are sanitized; a body
# longer than the announced size gets 400 and a wrong SHA-256 gets 422, both leaving nothing; SendFiles to the peer's
# `localsend:<fingerprint>` id delivers the file intact, and a peer announcing a fingerprint that is not its
# certificate's is refused at the handshake. Writes results.jsonl, peer.jsonl, peer-fake.jsonl, signals.txt, and
# linkd.log to $OUT (default ./artifacts/link-localsend).
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
OUT=${OUT:-$(pwd)/artifacts/link-localsend}
BIN=${LINK_BIN:-$ROOT/link/target/debug}
if [[ -z ${LINK_IN_NS:-} ]]; then
  rm -rf "$OUT"
  mkdir -p "$OUT"
  exec env LINK_IN_NS=1 OUT="$OUT" BIN="$BIN" unshare --user --map-root-user --net bash "$0"
fi

RUNTIME=$(mktemp -d /tmp/link-localsend.XXXX)
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
PEER_NS=$!
for _ in $(seq 50); do [[ $(readlink /proc/$PEER_NS/ns/net) != "$(readlink /proc/self/ns/net)" ]] && break; sleep 0.02; done
ip link set p0 netns "$PEER_NS"
ip addr add 10.78.0.1/24 dev d0
ip link set d0 up
ip route add 224.0.0.0/4 dev d0
in_peer() { nsenter -t "$PEER_NS" -n -- "$@"; }
in_peer ip link set lo up
in_peer ip addr add 10.78.0.2/24 dev p0
in_peer ip link set p0 up
in_peer ip route add 224.0.0.0/4 dev p0

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
DOWNLOADS=$RUNTIME/downloads
mkdir -p "$DOWNLOADS" "$RUNTIME/peer-in" "$RUNTIME/src"
XDG_STATE_HOME=$RUNTIME/desktop XDG_DOWNLOAD_DIR=$DOWNLOADS "$BIN/umbriel-linkd" > "$OUT/linkd.log" 2>&1 &
for _ in $(seq 100); do link CancelPairing > /dev/null 2>&1 && break; sleep 0.05; done
dbus-monitor --session "type='signal',interface='org.umbriel.Link1'" > "$OUT/signals.txt" 2>&1 &
wait_for 5 "dbus-monitor did not start" test -s "$OUT/signals.txt"

API=https://10.78.0.1:53317/api/localsend/v2
curl_peer() { in_peer curl -sk --max-time 150 "$@"; }
offered() { grep -A1 'member=TransferOffered' "$OUT/signals.txt" | grep -o '"[0-9a-f]\{32\}"' | tail -1 | tr -d '"'; }
offers() { grep -c 'member=TransferOffered' "$OUT/signals.txt" || true; }
finished() { grep -A2 'member=TransferFinished' "$OUT/signals.txt" | grep -A1 -F "\"$1\"" | grep -o 'string "[a-z-]*"' | tail -1 | cut -d'"' -f2; }

[[ $(prop LocalSendVisible) == "(<false>,)" ]] || fail "LocalSend is on by default"
curl_peer -o /dev/null "$API/info" && fail "something answers on 53317 before LocalSend is on"
record '{"step":"off-by-default"}'

openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 1 -subj /CN=e2e-peer \
  -keyout "$RUNTIME/peer.key" -out "$RUNTIME/peer.pem" 2> /dev/null
PEER_FP=$(openssl x509 -in "$RUNTIME/peer.pem" -outform DER | sha256sum | cut -d' ' -f1)
link SetLocalSendVisible true > /dev/null || fail "SetLocalSendVisible"
wait_for 10 "nothing answers on 53317" curl_peer -o "$RUNTIME/info.json" "$API/info"
FP=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["fingerprint"])' "$RUNTIME/info.json")
CERT_FP=$(in_peer openssl s_client -connect 10.78.0.1:53317 < /dev/null 2> /dev/null | openssl x509 -outform DER | sha256sum | cut -d' ' -f1)
[[ $FP == "$CERT_FP" ]] || fail "info says $FP, the certificate is $CERT_FP"
record "{\"step\":\"tls-fingerprint\",\"fingerprint\":\"$FP\",\"matches_certificate\":true}"

# Not through in_peer: $! must be the peer itself (nsenter execs it), so that kill reaches it.
nsenter -t "$PEER_NS" -n -- python3 "$ROOT/tests/e2e/localsend_peer.py" --cert "$RUNTIME/peer.pem" \
  --key "$RUNTIME/peer.key" --out "$RUNTIME/peer-in" > "$OUT/peer.jsonl" 2> "$OUT/peer.log" &
PEER=$!
wait_for 10 "the peer did not start" grep -q '"event": "ready"' "$OUT/peer.jsonl"
wait_for 10 "the daemon did not register with the announcing peer" grep -q '"event": "register"' "$OUT/peer.jsonl"
grep '"event": "register"' "$OUT/peer.jsonl" | grep -qF "\"fingerprint\": \"$FP\"" || fail "register carried another fingerprint"
wait_for 5 "Nearby does not list the peer: $(prop Nearby)" eval "prop Nearby | grep -qF 'localsend:$PEER_FP'"
link SetLocalSendVisible false > /dev/null
link SetLocalSendVisible true > /dev/null
wait_for 10 "the peer never heard the daemon's announcement" grep -q '"event": "announcement"' "$OUT/peer.jsonl"
grep '"event": "announcement"' "$OUT/peer.jsonl" | grep -qF "\"fingerprint\": \"$FP\"" || fail "the announcement carried another fingerprint"
record '{"step":"discovery","register":true,"nearby":true,"announcement_fingerprint_matches":true}'
wait_for 10 "LocalSend did not come back" curl_peer -o /dev/null "$API/info"

prepare() { # prepare NAME SIZE SHA256 OUTFILE: a prepare-upload in the background; its status lands in OUTFILE
  local body
  body=$(python3 -c 'import json,sys; print(json.dumps({"info": {"alias": "curl peer", "version": "2.1", "deviceModel": "curl", "deviceType": "headless", "fingerprint": sys.argv[4], "port": 53317, "protocol": "https", "download": False}, "files": {"f1": {"id": "f1", "fileName": sys.argv[1], "size": int(sys.argv[2]), "fileType": "application/octet-stream", "sha256": sys.argv[3]}}}))' "$1" "$2" "$3" "$PEER_FP")
  curl_peer -o "$4.json" -w '%{http_code}' -H 'Content-Type: application/json' -d "$body" "$API/prepare-upload" > "$4" &
}
upload() { # upload PREPARED DATAFILE: prints the upload's status
  local session token
  session=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["sessionId"])' "$1.json")
  token=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["files"]["f1"])' "$1.json")
  curl_peer -o /dev/null -w '%{http_code}' --data-binary "@$2" "$API/upload?sessionId=$session&fileId=f1&token=$token"
}
accept_next() { # accept_next COUNT: waits for the COUNTth offer and accepts it
  wait_for 10 "no TransferOffered" eval "(( \$(offers) >= $1 ))"
  link AcceptTransfer "'$(offered)'" > /dev/null || fail "AcceptTransfer"
}
sha() { sha256sum "$1" | cut -d' ' -f1; }

head -c 2097152 /dev/urandom > "$RUNTIME/src/photo.jpg"
prepare photo.jpg 2097152 "$(sha "$RUNTIME/src/photo.jpg")" "$RUNTIME/p1"
accept_next 1
grep -A2 'member=TransferOffered' "$OUT/signals.txt" | grep -qF "localsend:$PEER_FP" || fail "the offer does not name the peer"
wait_for 10 "prepare-upload did not return" test -s "$RUNTIME/p1"
[[ $(cat "$RUNTIME/p1") == 200 ]] || fail "prepare-upload: $(cat "$RUNTIME/p1")"
[[ $(upload "$RUNTIME/p1" "$RUNTIME/src/photo.jpg") == 200 ]] || fail "upload"
[[ $(sha "$DOWNLOADS/photo.jpg") == "$(sha "$RUNTIME/src/photo.jpg")" ]] || fail "the received file differs"
wait_for 5 "no TransferFinished done" eval "[[ \$(finished $(offered)) == done ]]"
record '{"step":"receive","bytes":2097152,"sha256_match":true,"consent":"TransferOffered, AcceptTransfer"}'

prepare nope.txt 4 "" "$RUNTIME/p2"
wait_for 10 "no second offer" eval '(( $(offers) >= 2 ))'
prepare other.txt 4 "" "$RUNTIME/p3"
wait_for 10 "the second prepare-upload was not refused" test -s "$RUNTIME/p3"
[[ $(cat "$RUNTIME/p3") == 409 ]] || fail "a second session got $(cat "$RUNTIME/p3")"
link DeclineTransfer "'$(offered)'" > /dev/null || fail "DeclineTransfer"
wait_for 10 "the declined prepare-upload did not return" test -s "$RUNTIME/p2"
[[ $(cat "$RUNTIME/p2") == 403 ]] || fail "a declined offer got $(cat "$RUNTIME/p2")"
record '{"step":"decline-and-busy","declined":403,"second_session":409}'

printf 'evil' > "$RUNTIME/src/evil"
n=2
for name in ../evil .bashrc; do
  n=$(( n + 1 ))
  prepare "$name" 4 "$(sha "$RUNTIME/src/evil")" "$RUNTIME/h$n"
  accept_next "$n"
  wait_for 10 "prepare-upload for $name did not return" test -s "$RUNTIME/h$n"
  [[ $(upload "$RUNTIME/h$n" "$RUNTIME/src/evil") == 200 ]] || fail "upload of $name"
done
[[ -f $DOWNLOADS/evil && -f $DOWNLOADS/bashrc && ! -e $RUNTIME/evil ]] || fail "hostile names: $(ls -A "$DOWNLOADS")"
record '{"step":"hostile-names","published":["evil","bashrc"]}'

head -c 8 /dev/urandom > "$RUNTIME/src/eight"
prepare long.bin 4 "" "$RUNTIME/o1"
accept_next 5
wait_for 10 "prepare-upload did not return" test -s "$RUNTIME/o1"
[[ $(upload "$RUNTIME/o1" "$RUNTIME/src/eight") == 400 ]] || fail "an oversized upload was not refused"
prepare wrong.bin 8 "$(printf '0%.0s' $(seq 64))" "$RUNTIME/o2"
accept_next 6
wait_for 10 "prepare-upload did not return" test -s "$RUNTIME/o2"
[[ $(upload "$RUNTIME/o2" "$RUNTIME/src/eight") == 422 ]] || fail "a wrong hash was not refused"
[[ ! -e $DOWNLOADS/long.bin && ! -e $DOWNLOADS/wrong.bin ]] || fail "a refused upload was published"
ls -A "$DOWNLOADS" | grep -q linkpart && fail "a refused upload left its part"
record '{"step":"size-and-hash-enforced","oversize":400,"wrong_sha256":422}'

head -c 1048576 /dev/urandom > "$RUNTIME/src/to-peer.bin"
SENT=$(python3 "$ROOT/tests/e2e/link_send_files.py" "localsend:$PEER_FP" "$RUNTIME/src/to-peer.bin") || fail "SendFiles"
wait_for 20 "the send never finished" eval "[[ -n \$(finished $SENT) ]]"
[[ $(finished "$SENT") == done ]] || fail "the send ended $(finished "$SENT")"
[[ $(sha "$RUNTIME/peer-in/to-peer.bin") == "$(sha "$RUNTIME/src/to-peer.bin")" ]] || fail "the peer got another file"
grep '"event": "upload"' "$OUT/peer.jsonl" | grep -qF "\"announced_sha256\": \"$(sha "$RUNTIME/src/to-peer.bin")\"" \
  || fail "the offer did not carry the file's SHA-256"
record '{"step":"send","bytes":1048576,"sha256_match":true}'

kill "$PEER"; wait "$PEER" 2> /dev/null || true
nsenter -t "$PEER_NS" -n -- python3 "$ROOT/tests/e2e/localsend_peer.py" --cert "$RUNTIME/peer.pem" --key "$RUNTIME/peer.key" \
  --out "$RUNTIME/peer-in" --alias "Impostor" --fingerprint "$(printf 'a%.0s' $(seq 64))" > "$OUT/peer-fake.jsonl" 2>&1 &
wait_for 10 "Nearby does not list the impostor" eval "prop Nearby | grep -qF 'localsend:$(printf 'a%.0s' $(seq 64))'"
FAKE=$(python3 "$ROOT/tests/e2e/link_send_files.py" "localsend:$(printf 'a%.0s' $(seq 64))" "$RUNTIME/src/to-peer.bin") \
  || fail "SendFiles to the impostor"
wait_for 20 "the send to the impostor never ended" eval "[[ -n \$(finished $FAKE) ]]"
[[ $(finished "$FAKE") == failed ]] || fail "a send to a wrong fingerprint ended $(finished "$FAKE")"
grep -q '"event": "prepare-upload"' "$OUT/peer-fake.jsonl" && fail "the impostor got the offer"
record '{"step":"fingerprint-pinned","impostor":"failed at the handshake"}'
cat "$OUT/results.jsonl"
echo "PASS; artifacts: $OUT"
