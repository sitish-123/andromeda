#!/usr/bin/env bash
# A window's title capsule shows its app icon: the .desktop file's Icon= looked up in the icon theme and drawn before
# the title (DESIGN.md "Overview v3" Completion).
set -euo pipefail

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
IMAGE="$ANDROMEDA_RUNTIME_DIR/icons.png"

data="$ANDROMEDA_RUNTIME_DIR/xdg-data"
mkdir -p "$data/applications" "$data/icons/hicolor/scalable/apps"
printf '[Desktop Entry]\nType=Application\nName=Icon Test\nExec=true\nIcon=overview-icon-test\n' \
  > "$data/applications/overview.icon.test.desktop"
cat > "$data/icons/hicolor/scalable/apps/overview-icon-test.svg" <<'SVG'
<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16"><rect width="16" height="16" fill="#ff00ff"/></svg>
SVG

cat >> "$ANDROMEDA_CONFIG" <<'EOF2'

[layout]
mode = "floating"

[overview]
style = "spaces"
titles = "always"
background_blur = false

[animation]
enabled = false
EOF2
"$ANDROMEDA" msg config-reload > /dev/null

APP_ID=overview.icon.test FILL_COLOR=0xFF0000FF "$ANDROMEDA_UNMAP_CLIENT" icon-window 500 400 > /dev/null 2>&1 &
for _ in $(seq 60); do
  [[ $("$ANDROMEDA" windows --json | jq 'length') -eq 1 ]] && break
  sleep 0.05
done
"$ANDROMEDA" settle
"$ANDROMEDA" msg overview-open > /dev/null
"$ANDROMEDA" settle
# The icon loads on a worker thread; the chrome redraws when it lands.
for _ in $(seq 40); do
  grim -o HEADLESS-1 "$IMAGE"
  read -r _ _ w h < <("$ANDROMEDA_PIXEL_PROBE" "$IMAGE" bbox 'r > 0.9 && b > 0.9 && g < 0.1')
  ((w >= 14 && w <= 18 && h >= 14 && h <= 18)) && { echo "the title capsule shows the app icon (${w}x$h)"; exit 0; }
  sleep 0.05
done
echo "no 16 px app icon in the title capsule (magenta box ${w}x$h)"
exit 1
