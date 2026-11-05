#!/usr/bin/env bash
# Quick Look over D-Bus: ShowFile on org.gnome.NautilusPreviewer2 opens an `andromeda-preview` window for a PNG,
# a PDF, a text file, an H.264 MP4 and a tagged FLAC; Close removes it. Fails when no preview window maps, a file is not decoded and shown
# ("shown <kind>" in the previewer log) within 5 s, the FLAC has no PipeWire stream, or the window stays after Close.
# k pauses and resumes the MP4; ANDROMEDA_PREVIEW_MEDIA=sw plays it again on the software path.
# Writes preview-{png,pdf,rs,mp4,flac,mp4-sw}.png and preview.log to $OUT (default ./artifacts/preview-show).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/preview-show}
PREVIEW=${PREVIEW:-$(cd "$(dirname "$0")/../.." && pwd)/andromeda-preview/build-debug/andromeda-preview}
PREVIEW_HELPER=${PREVIEW_HELPER:-$(cd "$(dirname "$0")/../.." && pwd)/andromeda-preview/build-debug/andromeda-preview-helper}
source "$(dirname "$0")/lib.sh"
boot_headless 1

# The previewer plays sound on the host PipeWire.
ln -s "$XDG_RUNTIME_DIR/pipewire-0" "$RUNTIME/pipewire-0"
mkdir -p "$RUNTIME/files"
magick -size 800x500 gradient:'#5147e5'-'#f5f5f7' "$RUNTIME/files/sample.png"
python3 -I - "$RUNTIME/files/sample.pdf" <<'PY'
import cairo, sys
s = cairo.PDFSurface(sys.argv[1], 595, 842)
c = cairo.Context(s)
for page in range(3):
    c.set_font_size(48); c.move_to(72, 144); c.show_text(f"Page {page + 1}"); c.show_page()
s.finish()
PY
# With a quiet AAC track: pause and resume run against the audio clock.
ffmpeg -loglevel error -f lavfi -i testsrc2=size=640x360:rate=30 -f lavfi -i 'sine=frequency=330,volume=0.05' -t 10 \
  -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest "$RUNTIME/files/sample.mp4"
# Quiet: the tone plays on the host's real output.
ffmpeg -loglevel error -f lavfi -i 'sine=frequency=440:duration=5,volume=0.05' -metadata title=Tone \
  -metadata artist=Tester -c:a flac "$RUNTIME/files/sample.flac"
printf 'fn main() {\n    println!("hello from quick look");\n}\n' > "$RUNTIME/files/sample.rs"

with_noctalia '
  wait_for() { for _ in $(seq 100); do eval "$2" && return 0; sleep 0.05; done; echo "FAIL: $1"; exit 1; }
  previews() { "$ANDROMEDA" windows --json | jq "[.[] | select(.app_id == \"andromeda-preview\")] | length"; }
  call() { gdbus call --session --dest org.gnome.NautilusPreviewer --object-path /org/gnome/NautilusPreviewer \
    --method org.gnome.NautilusPreviewer2.$1 "${@:2}" > /dev/null; }
  GST_DEBUG=va*:4 ANDROMEDA_PREVIEW_HELPER='"$PREVIEW_HELPER"' '"$PREVIEW"' > "$OUT/preview.log" 2>&1 &
  previewer=$!
  wait_for "previewer did not take its bus name" "gdbus introspect --session --dest org.gnome.NautilusPreviewer --object-path /org/gnome/NautilusPreviewer > /dev/null 2>&1"
  shown() { grep -c "shown $1" "$OUT/preview.log" || true; }
  # One keyboard for the whole run: a wtype per key alone adds and removes the device, and keys get dropped.
  wtype -s 3600000 &
  for pair in png:image pdf:pdf rs:text mp4:video flac:"audio Tone"; do
    kind=${pair%%:*} what=${pair##*:}
    call ShowFile "file://$RUNTIME/files/sample.$kind" "" false
    wait_for "no preview window for $kind" "(( \$(previews) == 1 ))"
    wait_for "$kind was not decoded and shown" "(( \$(shown \"$what\") >= 1 ))"
    grim "$OUT/preview-$kind.png"
    if [[ $kind == mp4 ]]; then
      wtype k
      wait_for "k did not pause the video" "grep -q \"playback paused\" \"$OUT/preview.log\""
      wtype k
      wait_for "k did not resume the video" "grep -q \"playback playing\" \"$OUT/preview.log\""
    fi
  done
  wait_for "no PipeWire stream for the FLAC" "pw-dump 2>/dev/null | grep -q \"\\\"Quick Look\\\"\""

  # The helper log names the decoder; a host without VA-API decodes in software.
  grep -o "vah264dec" "$OUT/preview.log" | head -n 1 || echo "decoder: software"
  call Close
  wait_for "preview window stayed after Close" "(( \$(previews) == 0 ))"

  # The software path (wl_shm frames), the one the window falls back to when the compositor refuses a dmabuf.
  kill "$previewer"; wait "$previewer" || true
  ANDROMEDA_PREVIEW_MEDIA=sw ANDROMEDA_PREVIEW_HELPER='"$PREVIEW_HELPER"' '"$PREVIEW"' > "$OUT/preview-sw.log" 2>&1 &
  previewer=$!
  wait_for "software previewer did not take its bus name" "gdbus introspect --session --dest org.gnome.NautilusPreviewer --object-path /org/gnome/NautilusPreviewer > /dev/null 2>&1"
  call ShowFile "file://$RUNTIME/files/sample.mp4" "" false
  wait_for "no software video frame" "grep -q \"shown video .* (software)\" \"$OUT/preview-sw.log\""
  grim "$OUT/preview-mp4-sw.png"
  call Close
  kill "$previewer"; wait "$previewer" || true
  echo PASS
'
