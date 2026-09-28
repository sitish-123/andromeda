#!/usr/bin/env bash
# The screen magnifier: three zoom-in steps show the part of the output the zoom query reports, scaled up to fill it,
# so the magnified frame matches the unmagnified one cropped to that view and enlarged, and differs from the whole
# output; zoom-reset brings the plain frame back. A red window with a blue subsurface over a green backdrop gives the
# frames detail to compare.
set -euo pipefail

readonly CLIENT="${UMBRIEL_SUBSURFACE_CLIENT:-./build-debug/tests/subsurface-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/zoom-client.log"
readonly PLAIN="$UMBRIEL_RUNTIME_DIR/zoom-plain.png"
readonly ZOOMED="$UMBRIEL_RUNTIME_DIR/zoom-zoomed.png"
readonly EXPECTED="$UMBRIEL_RUNTIME_DIR/zoom-expected.png"
readonly RESET="$UMBRIEL_RUNTIME_DIR/zoom-reset.png"

cat >> "$UMBRIEL_CONFIG" <<CONF

[animation]
duration_ms = 1
curve = "linear"

[colors]
backdrop = "#00FF00FF"
CONF
"$UMBRIEL" msg config-reload > /dev/null

"$CLIENT" "zoom-target" > "$CLIENT_LOG" 2>&1 &
for _ in $(seq 60); do
  grep -q '^mapped$' "$CLIENT_LOG" && break
  sleep 0.05
done
grep -q '^mapped$' "$CLIENT_LOG" || { echo "client never mapped: $(cat "$CLIENT_LOG")"; exit 1; }
"$UMBRIEL" settle
grim "$PLAIN"

zoom_field() { "$UMBRIEL" zoom --json | jq -r ".[0].$1"; }
for _ in 1 2 3; do "$UMBRIEL" msg zoom-in > /dev/null; done
"$UMBRIEL" settle
factor=$(zoom_field factor)
awk -v f="$factor" 'BEGIN {exit !(f > 1.95 && f < 1.96)}' || { echo "three zoom-in steps gave factor $factor"; exit 1; }
grim "$ZOOMED"

read -r x y w h <<< "$(zoom_field 'view | "\(.x) \(.y) \(.width) \(.height)"')"
read -r out_w out_h <<< "$(magick identify -format '%w %h' "$PLAIN")"
magick "$PLAIN" -crop "$(printf '%.0fx%.0f+%.0f+%.0f' "$w" "$h" "$x" "$y")" +repage -resize "${out_w}x${out_h}!" "$EXPECTED"
rmse() { { magick compare -metric RMSE "$1" "$2" null: 2>&1 || true; } | sed -n 's/.*(\(.*\))/\1/p'; }
magnified=$(rmse "$ZOOMED" "$EXPECTED")
unchanged=$(rmse "$ZOOMED" "$PLAIN")
awk -v m="$magnified" -v u="$unchanged" 'BEGIN {exit !(m < 0.05 && u > 3 * m)}' \
  || { echo "the magnified frame is not the view enlarged: vs expected $magnified, vs plain $unchanged"; exit 1; }

"$UMBRIEL" msg zoom-reset > /dev/null
"$UMBRIEL" settle
awk -v f="$(zoom_field factor)" 'BEGIN {exit !(f == 1)}' || { echo "zoom-reset left factor $(zoom_field factor)"; exit 1; }
grim "$RESET"
restored=$(rmse "$RESET" "$PLAIN")
awk -v r="$restored" 'BEGIN {exit !(r < 0.01)}' || { echo "zoom-reset did not bring the plain frame back ($restored)"; exit 1; }

echo "magnified view matched the enlarged crop ($magnified vs $unchanged unzoomed), and zoom-reset restored the frame"
