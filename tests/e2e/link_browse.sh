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
# 8. Photos is the phone's library by album (a folder stands in for Android's media store), and its files read back.
# 9. A name looked up once its folder's listing is stale is asked for alone (fs-stat), not by listing the folder.
# 10. Listing an album fills the freedesktop thumbnail cache: each PNG's thumbnail (fs-thumb) lands under the MD5 of
#     its file URI with Thumb::URI and Thumb::MTime, its picture the phone's; a file with no picture gets none.
# Every message is checked against protocol/link-v1/messages.cddl. Writes results.jsonl, tree.txt, transcript.jsonl,
# thumbnails.txt, linkd.log, mount.log, hold.log to $OUT (default ./artifacts/link-browse).
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
LIBRARY=$RUNTIME/library
mkdir -p "$LIBRARY/Camera" "$LIBRARY/Screenshots"
cp "$STORAGE/DCIM/Camera/IMG_0001.jpg" "$LIBRARY/Camera/"
python3 - "$LIBRARY/Screenshots" <<'PY'
import os, struct, sys, zlib
def png(path, seed):
    rows = b"".join(b"\0" + bytes((x * seed + y) % 256 for x in range(3 * 32)) for y in range(32))
    chunk = lambda kind, data: struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 32, 32, 8, 2, 0, 0, 0))
                           + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))
png(os.path.join(sys.argv[1], "Screenshot 1.png"), 3)
png(os.path.join(sys.argv[1], "Screenshot_2.png"), 5)
PY

CODE=$(link StartPairing | sed -E "s/^\('([0-9]+)', .*/\1/")
"$BIN/umbriel-link-phone" --state "$RUNTIME/phone" --name phone pair --code "$CODE" --addr "127.0.0.1:$PORT" > /dev/null \
  || fail "pairing"
hold() {
  RUST_LOG=info "$BIN/umbriel-link-phone" --state "$RUNTIME/phone" --name phone --transcript "$OUT/transcript.jsonl" \
    --browse-root "Storage=$STORAGE" --photos "$LIBRARY" hold >> "$OUT/hold.jsonl" 2>> "$OUT/hold.log" &
  HOLD=$!
  wait_for 20 "the phone never connected" connected
}
hold
XDG_CACHE_HOME=$RUNTIME/cache "$BIN/umbriel-link-mount" "$MNT" > "$OUT/mount.log" 2>&1 &
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
[[ $(ls "$MNT/phone/Photos" | tr '\n' ' ') == "Camera Screenshots " ]] || fail "Photos lists: $(ls "$MNT/phone/Photos")"
[[ -f $MNT/phone/Photos/Camera/IMG_0001.jpg ]] || fail "Photos does not show the camera album"
(cd "$MNT" && find . | sort) > "$OUT/tree.txt"
record '{"step":"lists-roots-and-folders","hidden_left_out":true}'

sha() { sha256sum < "$1" | cut -d' ' -f1; }
for file in DCIM/Camera/IMG_0001.jpg Download/big.bin Download/notes.txt; do
  [[ $(sha "$STORAGE/$file") == $(sha "$MNT/phone/Storage/$file") ]] || fail "$file reads back different"
done
[[ $(sha "$LIBRARY/Camera/IMG_0001.jpg") == $(sha "$MNT/phone/Photos/Camera/IMG_0001.jpg") ]] || fail "a photo reads back different"
record '{"step":"photos-library-by-album","albums":["Camera","Screenshots"],"sha256_match":true}'
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

hex() { printf %s "$1" | xxd -p | tr -d '\n'; }
sent() { grep -c "\"cbor\":\"[0-9a-f]*$(hex "$1")" "$OUT/transcript.jsonl" || true; }
# fs-list of exactly /Storage/Download: its CBOR text header (0x71, 17 bytes) then the path.
lists() { grep -c "\"cbor\":\"[0-9a-f]*$(hex fs-list)[0-9a-f]*71$(hex /Storage/Download)\"" "$OUT/transcript.jsonl" || true; }
sleep 6 # real time: the mount's listing of Download goes stale
LISTS=$(lists); STATS=$(sent fs-stat)
stat "$MNT/phone/Storage/Download/notes.txt" > /dev/null || fail "stat after the listing went stale"
(( $(sent fs-stat) > STATS )) || fail "the lookup did not ask for the one entry"
[[ $(lists) == "$LISTS" ]] || fail "the lookup listed the folder again"
record '{"step":"stale-lookup-asks-one-entry","message":"fs-stat"}'

ls "$MNT/phone/Photos/Screenshots" "$MNT/phone/Photos/Camera" > /dev/null
python3 - "$MNT/phone/Photos" "$LIBRARY" "$RUNTIME/cache/thumbnails/large" "$OUT/thumbnails.txt" <<'PY' || fail "thumbnails"
import hashlib, os, struct, sys, time, urllib.parse
mounted, library, cache, report = sys.argv[1:5]
def chunks(data):
    at, out = 8, []
    while at < len(data):
        (length,) = struct.unpack(">I", data[at:at + 4])
        out.append((data[at + 4:at + 8], data[at + 8:at + 8 + length]))
        at += 12 + length
    return out
lines = []
for album, name in (("Screenshots", "Screenshot 1.png"), ("Screenshots", "Screenshot_2.png")):
    path = os.path.join(mounted, album, name)
    uri = "file://" + urllib.parse.quote(path, safe="/!$&'()*+,:=@~")
    target = os.path.join(cache, hashlib.md5(uri.encode()).hexdigest() + ".png")
    for _ in range(100):
        if os.path.exists(target):
            break
        time.sleep(0.1)
    else:
        sys.exit(f"no thumbnail for {uri} at {target}")
    thumb = open(target, "rb").read()
    text = dict(data.split(b"\0", 1) for kind, data in chunks(thumb) if kind == b"tEXt")
    assert text[b"Thumb::URI"].decode() == uri, text
    assert int(text[b"Thumb::MTime"]) == int(os.stat(os.path.join(library, album, name)).st_mtime), text
    source = open(os.path.join(library, album, name), "rb").read()
    assert [c for c in chunks(thumb) if c[0] != b"tEXt"] == chunks(source), "the picture is not the phone's"
    assert oct(os.stat(target).st_mode & 0o777) == "0o600"
    lines.append(f"{uri} -> {os.path.basename(target)} {text}")
camera = "file://" + urllib.parse.quote(os.path.join(mounted, "Camera", "IMG_0001.jpg"), safe="/!$&'()*+,:=@~")
assert not os.path.exists(os.path.join(cache, hashlib.md5(camera.encode()).hexdigest() + ".png")), "a file with no picture"
open(report, "w").write("\n".join(lines) + "\n")
PY
(( $(sent fs-thumb) >= 3 )) || fail "the mount did not ask for each picture's thumbnail"
record '{"step":"album-thumbnails-cached","thumbnails":2,"text_chunks":["Thumb::URI","Thumb::MTime"],"no_picture_skipped":true}'

kill "$HOLD"; wait "$HOLD" 2>/dev/null || true
wait_for 15 "the disconnected phone stayed in the mount" eval '[[ ! -e "$MNT/phone" ]]'
record '{"step":"disconnected-phone-leaves"}'

"$BIN/umbriel-link-phone" check-transcript "$ROOT/protocol/link-v1/messages.cddl" "$OUT/transcript.jsonl" > "$OUT/schema.txt" \
  || fail "transcript violates the schema: $(cat "$OUT/schema.txt")"
record "{\"step\":\"schema\",\"result\":$(cat "$OUT/schema.txt")}"

cat "$OUT/results.jsonl"
echo "PASS; artifacts: $OUT"
