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
g++ -std=c++23 -I"$SRC/src" -I"$ROOT/libdesk-ui/src" -o "$RUNTIME/default-apps-set" \
  "$(dirname "$0")/default_apps_set.cpp" "$SRC/src/system/default_apps.cpp" "$ROOT/libdesk-ui/src/core/log.cpp"

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

# The page shows the effective default: the package's andromeda-mimeapps.list in an Andromeda session, until the
# user's own list sets one.
# The desktop-specific list wins over the plain one in the same directory; a default that is not installed is skipped.
SYS=$RUNTIME/system/applications
mkdir -p "$SYS" "$RUNTIME/no-config-dirs"
for id in dev.andromeda.Files other-files generic-files; do
  printf '[Desktop Entry]\nType=Application\nName=%s\nExec=true\n' "$id" > "$SYS/$id.desktop"
done
printf '[Default Applications]\ninode/directory=missing-files.desktop;dev.andromeda.Files.desktop\n' > "$SYS/andromeda-mimeapps.list"
printf '[Default Applications]\ninode/directory=generic-files.desktop\n' > "$SYS/mimeapps.list"
effective() {
  run env XDG_CURRENT_DESKTOP=andromeda XDG_CONFIG_DIRS="$RUNTIME/no-config-dirs" XDG_DATA_HOME="$RUNTIME/no-data-home" \
    XDG_DATA_DIRS="$RUNTIME/system" "$RUNTIME/default-apps-set" --effective inode/directory
}
[[ $(effective) == dev.andromeda.Files.desktop ]] || { echo "FAIL: the system default is not shown: '$(effective)'" >&2; exit 1; }
run "$RUNTIME/default-apps-set" "$MIMEAPPS" inode/directory other-files.desktop > /dev/null
[[ $(effective) == other-files.desktop ]] || { echo "FAIL: the user's default does not win: '$(effective)'" >&2; exit 1; }
echo "PASS effective default: desktop list over plain, uninstalled skipped, then the user's choice"
# A pick wins even over the user's own andromeda-mimeapps.list, which is read before ~/.config/mimeapps.list.
printf '[Default Applications]\ninode/directory=generic-files.desktop\n' > "$(dirname "$MIMEAPPS")/andromeda-mimeapps.list"
[[ $(effective) == generic-files.desktop ]] || { echo "FAIL: the user's desktop list is not read first: '$(effective)'" >&2; exit 1; }
picked=$(run env XDG_CURRENT_DESKTOP=andromeda XDG_CONFIG_DIRS="$RUNTIME/no-config-dirs" XDG_DATA_HOME="$RUNTIME/no-data-home" \
  XDG_DATA_DIRS="$RUNTIME/system" "$RUNTIME/default-apps-set" --pick inode/directory other-files.desktop)
[[ $picked == other-files.desktop ]] || { echo "FAIL: a pick loses to the user's desktop list: '$picked'" >&2; exit 1; }
echo "PASS a pick wins over the user's desktop-specific list"

with_noctalia '
  "$NOCTALIA" msg settings-open default-apps > /dev/null
  sleep 2 # real time: the settings window maps and paints
  grim "$OUT/default-apps.png"
'
[[ -s $OUT/default-apps.png ]] || { echo "no screenshot" >&2; exit 1; }
echo "PASS: mimeapps.list updated by default_apps::setDefault; artifacts: $OUT"
