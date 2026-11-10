#!/usr/bin/env bash
# Files design shots next to .agent/mockups/files-v2-live.html sections 1-3: the real shell (wallpaper, blur, scheme)
# with the section 1 content in Documents, in light and dark, icons, list and columns. Shots only; the mockup is the
# reference. Usage: tests/e2e/files_design_shots.sh; images in $OUT.
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/files-design}
source "$(dirname "$0")/lib.sh"
FILES=${FILES:-$ROOT/apps/files/build/andromeda-files}
POINTER=$ROOT/compositor/build-debug/tests/pointer-client
# Scale 2 on a 2560x1440 mode: 1280x720 logical, pixels 1:1 with the mockup rendered at device scale 2.
boot_headless 1
printf '\n[output.HEADLESS-1]\nmode = "2560x1440@60"\nscale = 2\n' >> "$RUNTIME/andromeda.toml"
mkdir -p "$OUT"

H=$RUNTIME/home
mkdir -p "$H/Desktop" "$H/Documents" "$H/Downloads" "$H/Pictures" "$H/Music" "$H/Videos" "$H/.config"
cat > "$H/.config/user-dirs.dirs" <<'DIRS'
XDG_DESKTOP_DIR="$HOME/Desktop"
XDG_DOCUMENTS_DIR="$HOME/Documents"
XDG_DOWNLOAD_DIR="$HOME/Downloads"
XDG_PICTURES_DIR="$HOME/Pictures"
XDG_MUSIC_DIR="$HOME/Music"
XDG_VIDEOS_DIR="$HOME/Videos"
DIRS
D=$H/Documents
mkdir -p "$D/Work" "$D/Taxes 2026" "$D/Andromeda" "$D/Old"
for f in "Work/brief.pdf" "Work/plan.md" "Taxes 2026/receipts.pdf" "Andromeda/logo.png" "report.pdf" "budget.ods" \
  "notes.md" "slides.odp" "contract.docx" "holiday.mov" "archive.zip"; do echo x > "$D/$f"; done
# A real 42 s clip when ffmpeg is there: the icon shows its frame and the duration chip (DESIGN "Files v2 icons").
if command -v ffmpeg > /dev/null; then
  ffmpeg -loglevel error -y -f lavfi -i "color=c=0x3b6fd8:s=320x200:d=42:r=1" -c:v libx264 -pix_fmt yuv420p "$D/holiday.mov"
fi
python3 -I - "$D" <<'PY'
import sys, zlib, struct
def png(path, w, h, a, b):
    rows = []
    for y in range(h):
        t = y / max(1, h - 1)
        px = bytes(int(a[i] + (b[i] - a[i]) * t) for i in range(3))
        rows.append(b"\0" + px * w)
    chunk = lambda k, d: struct.pack(">I", len(d)) + k + d + struct.pack(">I", zlib.crc32(k + d))
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                           + chunk(b"IDAT", zlib.compress(b"".join(rows))) + chunk(b"IEND", b""))
d = sys.argv[1]
png(d + "/launch-poster.png", 480, 320, (58, 59, 143), (224, 69, 123))
png(d + "/Screenshot 2026-11-07 at 09.41.png", 480, 300, (28, 36, 80), (81, 71, 229))
png(d + "/avatar.png", 256, 256, (247, 217, 191), (224, 69, 123))
png(d + "/Andromeda/cover.png", 320, 200, (13, 43, 85), (47, 208, 181))
PY

export FILES POINTER H D
with_noctalia '
  fail() { echo "FAIL: $*"; exit 1; }
  settle() { "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null; }
  keys() { "$POINTER" 1280 720 keyboard-only tap 119 pause 100 "$@" > /dev/null; }
  "$ANDROMEDA" msg config-reload > /dev/null
  for _ in $(seq 100); do "$ANDROMEDA" outputs --json | jq -e ".[0].scale == 2" > /dev/null && break; sleep 0.05; done
  "$ANDROMEDA" outputs --json | jq -e ".[0].scale == 2" > /dev/null || fail "the output did not take scale 2"
  "$NOCTALIA" msg config-set dock.enabled false > /dev/null || true
  "$NOCTALIA" msg theme-mode-set light > /dev/null
  HOME="$H" XDG_CONFIG_HOME="$H/.config" LC_ALL=C.UTF-8 ANDROMEDA_LOG_LEVEL=debug "$FILES" "$D" > "$OUT/files.log" 2>&1 &
  for _ in $(seq 100); do grep -q "first frame" "$OUT/files.log" 2> /dev/null && break; sleep 0.05; done
  grep -q "first frame" "$OUT/files.log" || fail "Files drew no first frame"
  for _ in $(seq 60); do grep -q "scheme light" "$OUT/files.log" && break; sleep 0.05; done
  grep -q "scheme light" "$OUT/files.log" || fail "Files did not follow the light scheme"
  sleep 0.5; settle; grim "$OUT/1-icons-light.png"
  keys mod control tap 3 mod none; sleep 0.5; settle; grim "$OUT/2-list-light.png"
  keys mod control tap 4 mod none; sleep 0.5; settle; grim "$OUT/3-columns-light.png"
  "$NOCTALIA" msg theme-mode-set dark > /dev/null
  for _ in $(seq 60); do grep -q "scheme dark" "$OUT/files.log" && break; sleep 0.05; done
  grep -q "scheme dark" "$OUT/files.log" || fail "Files did not follow the dark scheme"
  sleep 0.5; settle; grim "$OUT/4-columns-dark.png"
  keys mod control tap 3 mod none; sleep 0.5; settle; grim "$OUT/5-list-dark.png"
  keys mod control tap 2 mod none; sleep 0.5; settle; grim "$OUT/6-icons-dark.png"
  echo "pass files_design_shots"
'
