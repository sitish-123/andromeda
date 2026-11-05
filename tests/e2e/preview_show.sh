#!/usr/bin/env bash
# Quick Look over D-Bus: ShowFile on org.gnome.NautilusPreviewer2 opens an `andromeda-preview` window for a PNG,
# a PDF and a text file; Close removes it. Fails when no preview window maps, a file is not decoded and shown
# ("shown <kind>" in the previewer log) within 5 s, or the window stays after Close.
# Writes preview-{png,pdf,rs}.png and preview.log to $OUT (default ./artifacts/preview-show).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/preview-show}
PREVIEW=${PREVIEW:-$(cd "$(dirname "$0")/../.." && pwd)/andromeda-preview/build-debug/andromeda-preview}
PREVIEW_HELPER=${PREVIEW_HELPER:-$(cd "$(dirname "$0")/../.." && pwd)/andromeda-preview/build-debug/andromeda-preview-helper}
source "$(dirname "$0")/lib.sh"
boot_headless 1

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
printf 'fn main() {\n    println!("hello from quick look");\n}\n' > "$RUNTIME/files/sample.rs"

with_noctalia '
  wait_for() { for _ in $(seq 100); do eval "$2" && return 0; sleep 0.05; done; echo "FAIL: $1"; exit 1; }
  previews() { "$ANDROMEDA" windows --json | jq "[.[] | select(.app_id == \"andromeda-preview\")] | length"; }
  call() { gdbus call --session --dest org.gnome.NautilusPreviewer --object-path /org/gnome/NautilusPreviewer \
    --method org.gnome.NautilusPreviewer2.$1 "${@:2}" > /dev/null; }
  ANDROMEDA_PREVIEW_HELPER='"$PREVIEW_HELPER"' '"$PREVIEW"' > "$OUT/preview.log" 2>&1 &
  wait_for "previewer did not take its bus name" "gdbus introspect --session --dest org.gnome.NautilusPreviewer --object-path /org/gnome/NautilusPreviewer > /dev/null 2>&1"
  shown() { grep -c "shown $1" "$OUT/preview.log" || true; }
  for pair in png:image pdf:pdf rs:text; do
    kind=${pair%%:*} what=${pair##*:}
    call ShowFile "file://$RUNTIME/files/sample.$kind" "" false
    wait_for "no preview window for $kind" "(( \$(previews) == 1 ))"
    wait_for "$kind was not decoded and shown" "(( \$(shown $what) >= 1 ))"
    grim "$OUT/preview-$kind.png"
  done
  call Close
  wait_for "preview window stayed after Close" "(( \$(previews) == 0 ))"
  echo PASS
'
