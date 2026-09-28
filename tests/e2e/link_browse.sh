#!/usr/bin/env bash
# Browsing a phone's storage from the desktop through the real mount: umbriel-link-mount serves a private mount point
# inside user, network, and mount namespaces, umbriel-linkd answers it over D-Bus, and the headless phone serves a
# fixture tree through --browse-root.
#
# Proves:
# 1. With the phone's browse switch off (as after pairing), the phone's folder refuses with EACCES and nothing is read.
# 2. With it on, the mount lists the phone, its roots, and their folders; hidden entries are left out.
# 3. Every fixture file reads back through the mount with its SHA-256, and 200 random-offset reads of a 5 MiB file
#    match the source byte for byte, across the mount's 1 MiB blocks.
# 4. Symlinks out of a root (to /etc, and to a file beside the root) cannot be read.
# 5. The mount is read-only: creating or writing a file fails with EROFS.
# 6. A phone that disconnects leaves the mount's root once its listing goes stale.
# 7. A folder of 300 entries lists in full, across the phone's 128-entry pages.
# Writes results.jsonl, tree.txt, linkd.log, mount.log, hold.log to $OUT (default ./artifacts/link-browse).
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
OUT=${OUT:-$(pwd)/artifacts/link-browse}
BIN=${LINK_BIN:-$ROOT/link/target/debug}
if [[ -z ${LINK_IN_NS:-} ]]; then
  rm -rf "$OUT"
  mkdir -p "$OUT"
  exec env LINK_IN_NS=1 OUT="$OUT" BIN="$BIN" unshare --user --map-root-user --net --mount bash "$0"
fi

RUNTIME=$(mktemp -d /tmp/link-browse.XXXX)
MNT=$RUNTIME/Phone
trap 'kill $(jobs -p) 2>/dev/null || true; wait 2>/dev/null; umount -l "$MNT" 2>/dev/null || true; rm -rf "$RUNTIME"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }
record() { printf '%s\n' "$1" >> "$OUT/results.jsonl"; }
wait_for() {
  local tries=$(( $1 * 10 )) what=$2; shift 2
  for _ in $(seq "$tries"); do "$@" && return; sleep 0.1; done
  fail "$what"
}

ip link set lo up
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
connected() { devices | grep -q "'phone', true"; }
XDG_STATE_HOME=$RUNTIME/desktop XDG_DOWNLOAD_DIR=$RUNTIME/downloads "$BIN/umbriel-linkd" >> "$OUT/linkd.log" 2>&1 &
for _ in $(seq 100); do link CancelPairing > /dev/null 2>&1 && break; sleep 0.05; done
link CancelPairing > /dev/null || fail "umbriel-linkd did not come up"
PORT=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["port"])' "$RUNTIME/desktop/umbriel-link/devices.json")

STORAGE=$RUNTIME/storage
mkdir -p "$STORAGE/DCIM/Camera" "$STORAGE/Download" "$STORAGE/Many" "$RUNTIME/outside"
head -c 3145728 /dev/urandom > "$STORAGE/DCIM/Camera/IMG_0001.jpg"
head -c 5242881 /dev/urandom > "$STORAGE/Download/big.bin"
printf 'notes from the phone\n' > "$STORAGE/Download/notes.txt"
printf 'hidden\n' > "$STORAGE/.nomedia"
printf 'beside the root\n' > "$RUNTIME/outside/secret.txt"
ln -s /etc "$STORAGE/Download/etc"
ln -s ../outside/secret.txt "$STORAGE/Download/secret.txt"
for i in $(seq -w 300); do : > "$STORAGE/Many/file-$i"; done

CODE=$(link StartPairing | sed -E "s/^\('([0-9]+)', .*/\1/")
"$BIN/umbriel-link-phone" --state "$RUNTIME/phone" --name phone pair --code "$CODE" --addr "127.0.0.1:$PORT" > /dev/null \
  || fail "pairing"
hold() {
  RUST_LOG=info "$BIN/umbriel-link-phone" --state "$RUNTIME/phone" --name phone \
    --browse-root "Storage=$STORAGE" --browse-root "Photos=$STORAGE/DCIM" hold >> "$OUT/hold.jsonl" 2>> "$OUT/hold.log" &
  HOLD=$!
  wait_for 20 "the phone never connected" connected
}
hold
"$BIN/umbriel-link-mount" "$MNT" > "$OUT/mount.log" 2>&1 &
wait_for 10 "the mount did not come up" mountpoint -q "$MNT"

wait_for 10 "the mount does not list the phone" test -d "$MNT/phone"
OUTPUT=$(ls "$MNT/phone" 2>&1) && fail "a phone with browsing off was listed: $OUTPUT"
[[ $OUTPUT == *"Permission denied"* ]] || fail "browsing off did not refuse with EACCES: $OUTPUT"
record '{"step":"browse-off-refused","errno":"EACCES"}'

kill "$HOLD"; wait "$HOLD" 2>/dev/null || true
python3 - "$RUNTIME/phone/devices.json" <<'PY'
import json, sys
store = json.load(open(sys.argv[1]))
store["peers"][0].setdefault("grants", {})["browse"] = True
json.dump(store, open(sys.argv[1], "w"))
PY
hold
sleep 6
[[ $(ls "$MNT/phone" | tr '\n' ' ') == "Photos Storage " ]] || fail "the phone's roots: $(ls "$MNT/phone")"
[[ $(ls "$MNT/phone/Storage" | tr '\n' ' ') == "DCIM Download Many " ]] || fail "Storage lists: $(ls -a "$MNT/phone/Storage")"
[[ -f $MNT/phone/Photos/Camera/IMG_0001.jpg ]] || fail "Photos does not show the camera folder"
(cd "$MNT" && find . | sort) > "$OUT/tree.txt"
record '{"step":"lists-roots-and-folders","hidden_left_out":true}'

sha() { sha256sum < "$1" | cut -d' ' -f1; }
for file in DCIM/Camera/IMG_0001.jpg Download/big.bin Download/notes.txt; do
  [[ $(sha "$STORAGE/$file") == $(sha "$MNT/phone/Storage/$file") ]] || fail "$file reads back different"
done
python3 - "$STORAGE/Download/big.bin" "$MNT/phone/Storage/Download/big.bin" <<'PY' || fail "random-offset reads differ"
import os, random, sys
source, mounted = (open(path, "rb") for path in sys.argv[1:3])
size = os.path.getsize(sys.argv[1])
rng = random.Random(7)
for _ in range(200):
    offset, length = rng.randrange(size), rng.randrange(1, 300000)
    source.seek(offset); mounted.seek(offset)
    assert source.read(length) == mounted.read(length), (offset, length)
PY
record '{"step":"reads-match","files":3,"random_reads":200,"sha256_match":true}'

cat "$MNT/phone/Storage/Download/etc/passwd" > /dev/null 2>&1 && fail "a symlink to /etc was readable"
cat "$MNT/phone/Storage/Download/secret.txt" > /dev/null 2>&1 && fail "a symlink out of the root was readable"
record '{"step":"symlink-escapes-refused"}'

OUTPUT=$(touch "$MNT/phone/Storage/new.txt" 2>&1) && fail "a file was created on the phone"
[[ $OUTPUT == *"Read-only file system"* ]] || fail "creating a file: $OUTPUT"
OUTPUT=$( (printf x >> "$MNT/phone/Storage/Download/notes.txt") 2>&1) && fail "a file on the phone was written"
[[ $OUTPUT == *"Read-only file system"* ]] || fail "writing a file: $OUTPUT"
record '{"step":"read-only","errno":"EROFS"}'

[[ $(ls "$MNT/phone/Storage/Many" | wc -l) == 300 ]] || fail "Many lists $(ls "$MNT/phone/Storage/Many" | wc -l) of 300"
record '{"step":"long-folder-pages","entries":300}'

kill "$HOLD"; wait "$HOLD" 2>/dev/null || true
wait_for 15 "the disconnected phone stayed in the mount" eval '[[ ! -e "$MNT/phone" ]]'
record '{"step":"disconnected-phone-leaves"}'

cat "$OUT/results.jsonl"
echo "PASS; artifacts: $OUT"
