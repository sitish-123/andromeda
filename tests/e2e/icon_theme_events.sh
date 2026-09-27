#!/usr/bin/env bash
# The icon theme follows changes as they happen instead of on a 60 s poll: with the shell idle, installing a theme under
# ~/.local/share/icons makes the shell reload its icons within 2 s, and removing it again does the same. (Which theme is
# active is GSettings' call first, so the test changes what is installed rather than GTK's settings.ini.) Writes
# steps.txt and noctalia.log to $OUT (default ./artifacts/icon-theme-events).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/icon-theme-events}
source "$(dirname "$0")/lib.sh"
boot_headless 1
rm -f "$OUT/steps.txt"
mkdir -p "$RUNTIME/home/.local/share/icons"

with_noctalia '
  fail() { echo "FAIL: $*" >&2; exit 1; }
  step() { printf "%s\n" "$*" >> "$OUT/steps.txt"; }
  reloads() { grep -c "system icon theme changed" "$OUT/noctalia.log" || true; }
  wait_reload() {
    local want=$1 start=$SECONDS
    for _ in $(seq 20); do [[ $(reloads) -ge $want ]] && { echo $((SECONDS - start)); return; }; sleep 0.1; done
    fail "no icon reload within 2 s (have $(reloads), want $want)"
  }
  sleep 1 # real time: the shell has settled into idle
  before=$(reloads)

  THEME=$RUNTIME/home/.local/share/icons/E2ETheme
  mkdir -p "$THEME/scalable/apps"
  printf "[Icon Theme]\nName=E2ETheme\nInherits=hicolor\nDirectories=scalable/apps\n\n[scalable/apps]\nSize=48\nType=Scalable\n" > "$THEME/index.theme"
  took=$(wait_reload $((before + 1)))
  step "installed E2ETheme: reloaded within ${took}s"

  rm -rf "$THEME"
  took=$(wait_reload $((before + 2)))
  step "removed it: reloaded within ${took}s"
'
cat "$OUT/steps.txt"
echo "PASS; artifacts: $OUT"
