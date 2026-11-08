#!/usr/bin/env bash
# Files FB-GALLERY chrome (DESIGN.md "Files v2 immersive Gallery"): the capsules fade 2 s after the last pointer move,
# a move brings them back, F hides and shows them; the photo grows into the freed space. Fails when:
#   - Ctrl+4 does not open the Gallery,
#   - the chrome does not hide 2 s after the pointer stops, or a move or F does not bring it back,
#   - a hide or show reports missed frames (DESK_FRAME_STATS),
#   - I does not open the Info card with the photo's place from its GPS tag, or I does not close it,
#   - the Favourite button does not star the photo, or R does not turn the JPEG's Orientation tag a quarter,
#   - a double-click does not zoom to 100 % with the navigator, a wheel step does not zoom, a drag does not pan, or a
#     second double-click does not return to fit. (Pinch is not driven here: the virtual pointer has no gestures.)
#   - Space does not play a video in place with its scrubber, or the video does not end,
#   - a Live Photo pair (C_live.jpg + C_live.mov) is not one item whose motion plays muted on open,
#   - Space on a photo does not start the slideshow.
# Prints the Gallery speed numbers (next photo shown, full size) next to their budgets.
# Shots of the chrome shown and hidden go to $OUT (default ./artifacts/files-gallery).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/files-gallery}
source "$(dirname "$0")/lib.sh"
FILES=${FILES:-$ROOT/apps/files/build/andromeda-files}
POINTER=$ROOT/compositor/build-debug/tests/pointer-client
HOME_DIR=$RUNTIME/home
SRC=$HOME_DIR/Photos
boot_headless 1
mkdir -p "$OUT" "$SRC"
fail() { echo "FAIL: $*"; exit 1; }

# 12 landscape PNGs, each its own gradient.
python3 -I - "$SRC" <<'PY'
import sys, zlib, struct
root = sys.argv[1]
w, h = 480, 320
chunk = lambda k, d: struct.pack(">I", len(d)) + k + d + struct.pack(">I", zlib.crc32(k + d))
head = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
for i in range(12):
    a = (i * 37 % 256, i * 91 % 256, i * 53 % 256)
    rows = [b"\0" + bytes(int(a[c] * (1 - y / (h - 1)) + (255 - a[c]) * y / (h - 1)) for c in range(3)) * w for y in range(h)]
    with open(f"{root}/IMG_{i:02d}.png", "wb") as f:
        f.write(head + chunk(b"IDAT", zlib.compress(b"".join(rows), 1)) + chunk(b"IEND", b""))
PY
# A_kathmandu.jpg sorts first: a real JPEG (cjpeg) with an EXIF block: camera, exposure, Orientation 1, GPS in Kathmandu.
python3 -I - "$SRC" <<'PY'
import struct, subprocess, sys
root = sys.argv[1]
# Larger than the stage, so 100 % is a real zoom in.
w, h = 2400, 1600
red = bytes(x * 255 // w for x in range(w))
rows = bytearray()
for y in range(h):
    row = bytearray(w * 3)
    row[0::3] = red
    row[1::3] = bytes([y * 255 // h]) * w
    row[2::3] = b"\xa0" * w
    rows += row
jpeg = subprocess.run(["cjpeg", "-quality", "85"], input=b"P6 %d %d 255\n" % (w, h) + bytes(rows), capture_output=True, check=True).stdout
def ifd(entries, base, nxt=0):
    # entries: (tag, type, count, value bytes); values over 4 bytes go after the IFD, offsets relative to the TIFF start.
    head = struct.pack("<H", len(entries))
    data = b""
    data_at = base + 2 + 12 * len(entries) + 4
    for tag, typ, count, value in entries:
        if len(value) <= 4:
            head += struct.pack("<HHI", tag, typ, count) + value.ljust(4, b"\0")
        else:
            head += struct.pack("<HHII", tag, typ, count, data_at + len(data))
            data += value
    return head + struct.pack("<I", nxt) + data
rat = lambda *pairs: b"".join(struct.pack("<II", n, d) for n, d in pairs)
asc = lambda t: t.encode() + b"\0"
make, model = asc("Google"), asc("Pixel 9 Pro")
ifd0_entries = lambda exif_at, gps_at: [(0x010F, 2, len(make), make), (0x0110, 2, len(model), model), (0x0112, 3, 1, struct.pack("<H", 1)),
                                        (0x8769, 4, 1, struct.pack("<I", exif_at)), (0x8825, 4, 1, struct.pack("<I", gps_at))]
size0 = len(ifd(ifd0_entries(0, 0), 8))
date = asc("2026:10:02 07:12:14")
exif_entries = [(0x829A, 5, 1, rat((1, 120))), (0x829D, 5, 1, rat((17, 10))), (0x8827, 3, 1, struct.pack("<H", 50)),
                (0x9003, 2, len(date), date), (0x920A, 5, 1, rat((25, 1)))]
exif_at = 8 + size0
size_exif = len(ifd(exif_entries, exif_at))
gps_at = exif_at + size_exif
gps_entries = [(1, 2, 2, b"N\0"), (2, 5, 3, rat((27, 1), (42, 1), (54, 1))), (3, 2, 2, b"E\0"), (4, 5, 3, rat((85, 1), (18, 1), (54, 1)))]
tiff = b"II*\0" + struct.pack("<I", 8) + ifd(ifd0_entries(exif_at, gps_at), 8) + ifd(exif_entries, exif_at) + ifd(gps_entries, gps_at)
app1 = b"Exif\0\0" + tiff
with open(f"{root}/A_kathmandu.jpg", "wb") as f:
    f.write(jpeg[:2] + b"\xff\xe1" + struct.pack(">H", len(app1) + 2) + app1 + jpeg[2:])
PY
# B_clip.mp4: a 2 s video with sound; C_live.jpg + C_live.mov: a Live Photo pair (one Gallery item, motion muted).
ffmpeg -loglevel error -f lavfi -i testsrc=duration=2:size=640x360:rate=30 -f lavfi -i sine=duration=2 \
  -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest "$SRC/B_clip.mp4"
ffmpeg -loglevel error -f lavfi -i testsrc2=duration=1:size=480x320:rate=30 -frames:v 1 -update 1 "$SRC/C_live.jpg"
ffmpeg -loglevel error -f lavfi -i testsrc2=duration=1:size=480x320:rate=30 -c:v libx264 -pix_fmt yuv420p "$SRC/C_live.mov"
touch -d '2026-10-02 07:00' "$SRC"/A_kathmandu.jpg "$SRC"/B_clip.mp4   # one day; the rest today: a filmstrip divider
orientation() { # the IFD0 Orientation value of A_kathmandu.jpg
  python3 -I - "$SRC/A_kathmandu.jpg" <<'PY'
import struct, sys
b = open(sys.argv[1], "rb").read()
t = b.index(b"Exif\0\0") + 6
ifd = t + struct.unpack_from("<I", b, t + 4)[0]
for i in range(struct.unpack_from("<H", b, ifd)[0]):
    tag, typ, count, value = struct.unpack_from("<HHIH", b, ifd + 2 + 12 * i)
    if tag == 0x0112:
        print(value)
PY
}

bus_info=$(dbus-daemon --session --fork --print-address=1 --print-pid=1)
BUS=$(sed -n 1p <<< "$bus_info")
BUS_PID=$(sed -n 2p <<< "$bus_info")
trap 'kill "$BUS_PID" 2> /dev/null || true; kill $(jobs -p) 2> /dev/null || true; wait 2> /dev/null; rm_runtime' EXIT
run env DBUS_SESSION_BUS_ADDRESS="$BUS" HOME="$HOME_DIR" XDG_STATE_HOME="$HOME_DIR/.local/state" \
  XDG_CONFIG_HOME="$HOME_DIR/.config" XDG_CACHE_HOME="$HOME_DIR/.cache" LC_ALL=C.UTF-8 NOCTALIA_LOG_LEVEL=debug \
  DESK_FRAME_STATS=1 DESK_FILES_GEOMETRY=1 ANDROMEDA_PREVIEW_HELPER="$ROOT/apps/files/build/subprojects/andromeda-preview/andromeda-preview-helper" \
  "$FILES" "$SRC" > "$OUT/files.log" 2>&1 &

wait_log() {
  for _ in $(seq "${3:-100}"); do
    grep -qE "$2" < <(tail -n +"$(($1 + 1))" "$OUT/files.log") && return 0
    sleep 0.05
  done
  return 1
}
mark() { wc -l < "$OUT/files.log"; }
keys() { run "$POINTER" 1280 720 keyboard-only tap 119 pause 100 "$@" > /dev/null; }
shot() { run "$ANDROMEDA" settle > /dev/null; run grim "$OUT/$1.png"; }
missed_since() { tail -n +"$(($1 + 1))" "$OUT/files.log" | grep -oE "anim .* missed [0-9]+" | grep -oE '[0-9]+$' | paste -sd+ | bc; }

wait_log 0 "first frame at" 200 || fail "the window drew no first frame"
wait_log 0 "listed [0-9]+ in" 200 || fail "the folder did not list"
run "$ANDROMEDA" settle > /dev/null

m=$(mark); keys mod control tap 5 mod none   # Ctrl+4
wait_log "$m" "view gallery" || fail "Ctrl+4 did not open the Gallery"
run "$POINTER" 1280 720 move 640 360 pause 50 move 650 370 > /dev/null
run "$ANDROMEDA" settle > /dev/null
shot 1-chrome-shown

m=$(mark)
wait_log "$m" "gallery chrome hidden" 80 || fail "the chrome did not hide 2 s after the pointer stopped"
m=$(($(grep -nE "gallery chrome hidden" "$OUT/files.log" | tail -n 1 | cut -d: -f1) - 1))
wait_log "$m" "anim .* frames" 60 || fail "the hide reported no frame stats"
shot 2-chrome-hidden
hide_missed=$(missed_since "$m")

m=$(mark); run "$POINTER" 1280 720 move 600 300 > /dev/null
wait_log "$m" "gallery chrome shown" || fail "a pointer move did not bring the chrome back"
wait_log "$m" "anim .* frames" 60 || fail "the show reported no frame stats"
show_missed=$(missed_since "$m")

m=$(mark); keys tap 33                        # F
wait_log "$m" "gallery chrome hidden" || fail "F did not hide the chrome"
m=$(mark); keys tap 33
wait_log "$m" "gallery chrome shown" || fail "F did not bring the chrome back"

# Info card (mockup section 15): I opens it; the place comes from the GPS tag through the bundled offline map data.
m=$(mark); keys tap 23                        # I
wait_log "$m" "gallery info shown" || fail "I did not open the Info card"
wait_log "$m" "gallery info for A_kathmandu.jpg map Kathmandu" 100 || fail "the Info card did not place the photo in Kathmandu"
run "$POINTER" 1280 720 move 640 360 > /dev/null
shot 3-info
m=$(mark); keys tap 23
wait_log "$m" "gallery info hidden" || fail "a second I did not close the Info card"

# Favourite: the action button stars the photo; R rotates the JPEG a quarter without loss (Orientation 1 -> 6).
run "$POINTER" 1280 720 move 600 300 > /dev/null
wait_log 0 "geometry gallery-action favourite " 60 || fail "no geometry for the Favourite button"
read -r fx fy <<< "$(grep -E "geometry gallery-action favourite " "$OUT/files.log" | tail -n 1 | awk '{print $(NF-1), $NF}')"
w=$(run "$ANDROMEDA" windows --json | jq -c '[.. | objects | select(.app_id? == "dev.andromeda.Files")][0]')
m=$(mark); run "$POINTER" 1280 720 move $(($(jq .x <<< "$w") + fx)) $(($(jq .y <<< "$w") + fy)) click 272 > /dev/null
wait_log "$m" "gallery action favourite" || fail "a press on Favourite reached no handler"
wait_log "$m" "starred 1 items" || fail "Favourite did not star the photo"
shot 3b-starred
[[ $(orientation) == 1 ]] || fail "the fixture does not start at Orientation 1"
m=$(mark); keys tap 19                        # R
for _ in $(seq 40); do [[ $(orientation) == 6 ]] && break; sleep 0.05; done
[[ $(orientation) == 6 ]] || fail "R did not rotate the JPEG to Orientation 6"
shot 3c-rotated

# Zoom (mockup section 15 right): double-click = 100 % at the pointer, the wheel zooms, a drag pans, double-click fits.
px=$(($(jq .x <<< "$w") + 300)); py=$(($(jq .y <<< "$w") + 330))
m=$(mark); run "$POINTER" 1280 720 move "$px" "$py" click 272 pause 80 click 272 > /dev/null
wait_log "$m" "gallery zoom 100$" || fail "a double-click did not zoom to 100 %"
shot 4-zoom-100
m=$(mark); run "$POINTER" 1280 720 move "$px" "$py" notch -1 > /dev/null
wait_log "$m" "gallery zoom 1[2-9][0-9]$" || fail "a wheel step did not zoom in"
m=$(mark); run "$POINTER" 1280 720 move "$px" "$py" press 272 move $((px + 60)) $((py + 40)) pause 30 move $((px + 120)) $((py + 80)) release 272 > /dev/null
wait_log "$m" "gallery pan " || fail "a drag did not pan the zoomed photo"
m=$(mark); run "$POINTER" 1280 720 move "$px" "$py" pause 500 click 272 pause 80 click 272 > /dev/null
wait_log "$m" "gallery zoom fit" || fail "a second double-click did not return to fit"

# Video in place (mockup section 15 middle): Right to the clip, Space plays it with its scrubber, it ends by itself.
m=$(mark); keys tap 106                       # Right
wait_log "$m" "gallery 2 of 15" || fail "Right did not go to the clip (or the Live Photo video was not folded into its photo)"
m=$(mark); keys tap 57                        # Space
wait_log "$m" "gallery video playing B_clip.mp4" 100 || fail "Space did not play the video"
wait_log "$m" "gallery video B_clip.mp4 640x360" 100 || fail "the video showed no frame"
shot 5-video
wait_log "$m" "gallery video ended" 120 || fail "the video did not end"

# Live Photo: its motion plays once on open, muted.
m=$(mark); keys tap 106
wait_log "$m" "gallery 3 of 15" || fail "Right did not go to the Live Photo"
wait_log "$m" "gallery video playing C_live.mov muted" 100 || fail "the Live Photo's motion did not play muted on open"
wait_log "$m" "gallery video ended" 100 || fail "the Live Photo's motion did not end"
shot 6-live-photo

# Slideshow: Space on a photo moves on by itself.
m=$(mark); keys tap 57
wait_log "$m" "gallery slideshow on" || fail "Space on a photo did not start the slideshow"
wait_log "$m" "gallery 4 of 15" 100 || fail "the slideshow did not move to the next photo"
m=$(mark); keys tap 57
wait_log "$m" "gallery slideshow off" || fail "a second Space did not stop the slideshow"

# Speed (DESIGN.md "Files v2" Gallery budgets, release): next photo from its cached thumbnail, full size.
shown=$(grep -oE "gallery photo shown in [0-9.]+" "$OUT/files.log" | tail -n +2 | grep -oE '[0-9.]+$' | sort -g | tail -n 1)
full=$(grep -oE "gallery photo full size in [0-9.]+" "$OUT/files.log" | tail -n +2 | grep -oE '[0-9.]+$' | sort -g | tail -n 1)
echo "next photo shown (worst) ${shown:-?} ms, budget 16 ms; full size (worst) ${full:-?} ms, budget 150 ms (release budgets)"

echo "hide missed frames ${hide_missed:-0}, show missed frames ${show_missed:-0}"
[[ ${hide_missed:-0} -eq 0 && ${show_missed:-0} -eq 0 ]] || fail "the chrome motion missed frames"
echo "pass files_gallery"
