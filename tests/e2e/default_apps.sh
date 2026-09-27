#!/usr/bin/env bash
# Default Apps settings read/write $XDG_CONFIG_HOME/mimeapps.list: default_apps_set (linked against the same
# default_apps.cpp the settings page's control calls) sets a web-browser default, asserting the file's
# [Default Applications] section gained "x-scheme-handler/http=test-browser.desktop;" while leaving the rest of an
# existing file untouched. Also opens Settings > Default Applications in the running Noctalia fork and screenshots
# it. Writes artifacts to $OUT (default ./artifacts/default-apps).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/default-apps}
source "$(dirname "$0")/lib.sh"
boot_headless 1

SRC=$ROOT/shell
g++ -std=c++23 -I"$SRC/src" -o "$RUNTIME/default-apps-set" \
  "$(dirname "$0")/default_apps_set.cpp" "$SRC/src/system/default_apps.cpp" "$SRC/src/core/log.cpp"

MIMEAPPS="$RUNTIME/home/.config/mimeapps.list"
mkdir -p "$(dirname "$MIMEAPPS")"
cat > "$MIMEAPPS" <<'EOF'
[Default Applications]
text/plain=other-editor.desktop;

[Added Associations]
text/plain=other-editor.desktop;code.desktop;
EOF
before=$(cat "$MIMEAPPS")

run "$RUNTIME/default-apps-set" "$MIMEAPPS" "x-scheme-handler/http" "test-browser.desktop" | tee "$OUT/default-apps-set.txt"
grep -q "^PASS" "$OUT/default-apps-set.txt" && ! grep -q "^FAIL" "$OUT/default-apps-set.txt" \
  || { echo "default-apps-set reported a failure" >&2; exit 1; }

after=$(cat "$MIMEAPPS")
[[ "$before" != "$after" ]] || { echo "FAIL: mimeapps.list did not change" >&2; exit 1; }
grep -qx "x-scheme-handler/http=test-browser.desktop;" "$MIMEAPPS" \
  || { echo "FAIL: missing new default in mimeapps.list" >&2; exit 1; }
grep -qx "text/plain=other-editor.desktop;" "$MIMEAPPS" \
  || { echo "FAIL: existing default was clobbered" >&2; exit 1; }
grep -q "code.desktop" "$MIMEAPPS" || { echo "FAIL: unrelated [Added Associations] section was lost" >&2; exit 1; }
cp "$MIMEAPPS" "$OUT/mimeapps.list"

with_noctalia '
  "$NOCTALIA" msg settings-open default-apps > /dev/null
  sleep 2 # real time: the settings window maps and paints
  grim "$OUT/default-apps.png"
'
[[ -s $OUT/default-apps.png ]] || { echo "no screenshot" >&2; exit 1; }
echo "PASS: mimeapps.list updated by default_apps::setDefault; artifacts: $OUT"
