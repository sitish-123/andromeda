#!/usr/bin/env bash
# A black window on a black desktop keeps an edge: a one-device-pixel white highlight inside its outline, lit from the top.
set -euo pipefail
cat >>"$ANDROMEDA_CONFIG" <<'EOF'
[animation]
enabled = false
[appearance]
scheme = "dark"
border_width = 0
outer_border_width = 0
window_edge = true
corner_radius = 12
[appearance.shadow]
enabled = false
[appearance.title_bar]
mode = "off"
[output.HEADLESS-1]
scale = 1
[[window_rule]]
match.title = "^hairline$"
default_floating = true
default_floating_size_px = { width = 300, height = 200 }
default_position = { x = 100, y = 100, anchor = "top_left" }
EOF
"$ANDROMEDA" msg config-reload >/dev/null
FILL_COLOR=0xFF000000 "$ANDROMEDA_UNMAP_CLIENT" hairline 300 200 >"$ANDROMEDA_RUNTIME_DIR/hairline.log" 2>&1 &
for _ in $(seq 60); do
  grep -q '^mapped$' "$ANDROMEDA_RUNTIME_DIR/hairline.log" && break
  sleep 0.05
done
grep -q '^mapped$' "$ANDROMEDA_RUNTIME_DIR/hairline.log" || { echo "hairline client never mapped"; exit 1; }

for scale in 1 2; do
  sed -i "s/^scale = .*/scale = $scale/" "$ANDROMEDA_CONFIG"
  "$ANDROMEDA" msg config-reload >/dev/null
  "$ANDROMEDA" settle >/dev/null
  image=$ANDROMEDA_RUNTIME_DIR/hairline-$scale.png
  grim -o HEADLESS-1 "$image"
  # ANDROMEDA_SHOTS keeps the shots for a look by eye.
  [[ -n ${ANDROMEDA_SHOTS:-} ]] && cp "$image" "$ANDROMEDA_SHOTS/"
  probe() { "$ANDROMEDA_PIXEL_PROBE" "$image" max "$1" | cut -d' ' -f1; }
  # Window 300x200 at 100,100: top edge row, left edge at mid height, bottom edge row, and pixels beside them.
  top=$(probe "40x1+$((200 * scale))+$((100 * scale))")
  side=$(probe "1x10+$((100 * scale))+$((140 * scale))")
  bottom=$(probe "40x1+$((200 * scale))+$((300 * scale - 1))")
  inside=$(probe "40x1+$((200 * scale))+$((100 * scale + 1))")
  outside=$(probe "40x1+$((200 * scale))+$((100 * scale - 1))")
  echo "scale $scale: top $top side $side bottom $bottom inside $inside outside $outside"
  # A highlight lit from above: brightest on top (12 % white is about 31), fading down the sides, one device pixel
  # wide at any scale (the next pixel in is untouched), never a solid ring.
  if ((top < 20 || top > 40 || side >= top || side < 4 || bottom >= side || inside != 0 || outside != 0)); then
    echo "window highlight at scale $scale: top $top side $side bottom $bottom inside $inside outside $outside"
    exit 1
  fi
done
