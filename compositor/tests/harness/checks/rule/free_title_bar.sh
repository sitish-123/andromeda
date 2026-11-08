#!/usr/bin/env bash
# Automatic title bars: a Free desktop shows them, also on windows it already had; Scrolling keeps them off.
set -euo pipefail
source "$(dirname "$ANDROMEDA_HARNESS_LIB")/title_bar.sh"
title_bar_config
sed -i '/^\[appearance.title_bar\]$/ { n; s/mode = "[^"]*"/mode = "auto"/; }' "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null

spawn() { env XDG_DECORATION=server "$ANDROMEDA_FRACTIONAL_CLIENT" "$1" 480 300 > /dev/null 2>&1 & }

spawn free-scrolling
title_bar_wait_field free-scrolling title_bar false

printf '\n[layout]\nmode = "floating"\n' >> "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null
spawn free-floating
title_bar_wait_field free-floating title_bar true
title_bar_wait_field free-scrolling title_bar true
echo "automatic title bars show on a Free desktop only"
