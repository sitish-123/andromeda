#!/usr/bin/env bash
# S7b live check: the shell file dialog (grid view) takes thumbnails from the shared freedesktop cache.
#   - photo.png has a valid seeded entry in ~/.cache/thumbnails/large: the dialog uses it and decodes nothing itself,
#   - clip.mp4 has none: the dialog asks Thumbnailer1 (andromeda-thumbnailer, which runs the host's
#     ffmpegthumbnailer), and the shared entry appears. (The host has no PDF thumbnailer, so a PDF stays a glyph.)
# Fails when the video entry does not appear within 5 s, or the shell writes its own WebP cache for either file.
# Writes file-dialog.png and the logs to $OUT (default ./artifacts/file-dialog-thumbnails).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/file-dialog-thumbnails}
source "$(dirname "$0")/lib.sh"
THUMBNAILER=${THUMBNAILER:-$ROOT/andromeda-preview/build-debug/andromeda-thumbnailer}
boot_headless 1

# The thumbnailer sandboxes each job with systemd-run --user: reach the real user manager.
ln -s "$XDG_RUNTIME_DIR/systemd" "$RUNTIME/systemd"
docs=$RUNTIME/home/docs
mkdir -p "$docs" "$RUNTIME/home/.cache/thumbnails/large"
magick -size 800x500 gradient:'#5147e5'-'#f5f5f7' "$docs/photo.png"
ffmpeg -loglevel error -f lavfi -i testsrc2=size=640x360:rate=30 -t 2 -c:v libx264 -pix_fmt yuv420p "$docs/clip.mp4"
# Seeds the shared entry for photo.png: a 256 px PNG with the Thumb::URI and Thumb::MTime the spec requires.
magick "$docs/photo.png" -thumbnail 256x256 "$RUNTIME/seed.png"
python3 -I - "$docs/photo.png" "$RUNTIME/seed.png" "$RUNTIME/home/.cache/thumbnails/large" <<'PY'
import hashlib, os, struct, sys, urllib.parse, zlib
src, seed, large = sys.argv[1:]
uri = "file://" + urllib.parse.quote(src)
def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)
png = open(seed, "rb").read()
ihdr_end = 8 + 25
text = b"".join(chunk(b"tEXt", k + b"\0" + v.encode()) for k, v in
                ((b"Thumb::URI", uri), (b"Thumb::MTime", str(int(os.stat(src).st_mtime)))))
open(os.path.join(large, hashlib.md5(uri.encode()).hexdigest() + ".png"), "wb").write(png[:ihdr_end] + text + png[ihdr_end:])
PY
video_entry=$(python3 -I -c 'import hashlib, sys, urllib.parse; print(hashlib.md5(("file://" + urllib.parse.quote(sys.argv[1])).encode()).hexdigest())' "$docs/clip.mp4")

export THUMBNAILER docs video_entry
ANDROMEDA_SETTINGS_TEST_MODAL=file ANDROMEDA_SETTINGS_TEST_DIR=$docs with_noctalia '
  wait_for() { for _ in $(seq 100); do eval "$2" && return 0; sleep 0.05; done; echo "FAIL: $1"; exit 1; }
  "$THUMBNAILER" > "$OUT/thumbnailer.log" 2>&1 &
  # The private bus has no activation file: the dialog must find the name already owned.
  wait_for "the thumbnailer did not take its bus name" \
    "gdbus introspect --session --dest org.freedesktop.thumbnails.Thumbnailer1 --object-path /org/freedesktop/thumbnails/Thumbnailer1 > /dev/null 2>&1"
  # The hook runs on a second settings-open while the window is open.
  "$NOCTALIA" msg settings-open about > /dev/null
  sleep 0.8
  "$NOCTALIA" msg settings-open about > /dev/null
  sleep 0.8
  grim "$OUT/opened.png"
  shared=$HOME/.cache/thumbnails
  wait_for "Thumbnailer1 wrote no shared entry for clip.mp4" \
    "compgen -G \"$shared/*/$video_entry.png\" > /dev/null"
  sleep 1
  grim "$OUT/file-dialog.png"
  # No own cache dir at all is the pass case; find fails on it, so it must not end the script.
  own=$( { find "$HOME/.cache/noctalia/thumbnails" -name "*.webp" 2> /dev/null || true; } | wc -l)
  (( own == 0 )) || { echo "FAIL: the shell wrote $own WebP thumbnails of its own"; exit 1; }
'
echo "PASS; artifacts: $OUT"
