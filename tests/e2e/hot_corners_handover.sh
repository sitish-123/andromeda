#!/usr/bin/env bash
# On Umbriel the compositor owns hot corners. A shell started with its own [hot_corners] configured hands them over
# once: the compositor's hot_corners settings then carry the translated actions (launcher becomes
# "shell:panel-toggle launcher", overview becomes "overview-toggle") with the shell's delay, and the shell turns its
# own corners off. Started again, the shell leaves the compositor's corners as they are. Writes settings.txt and
# steps.txt to $OUT (default ./artifacts/hot-corners-handover).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/hot-corners-handover}
source "$(dirname "$0")/lib.sh"
boot_headless 1
rm -f "$OUT/steps.txt" "$OUT/settings.txt"
cat >> "$RUNTIME/home/.config/noctalia/config.toml" <<'TOML'

[hot_corners]
enabled = true
delay_ms = 250

[hot_corners.top_left]
action = "launcher"

[hot_corners.bottom_right]
action = "overview"
TOML

with_noctalia '
  fail() { echo "FAIL: $*" >&2; exit 1; }
  step() { printf "%s\n" "$*" >> "$OUT/steps.txt"; }
  setting() { run_client settings-state | grep "^$1=" | cut -d= -f2- | sed "s/ customized$//"; }
  run_client() { "$DESKTOP_CLIENT" "$@"; }
  expect() {
    for _ in $(seq 50); do [[ $(setting "$1") == "$2" ]] && return; sleep 0.1; done
    fail "compositor $1 is \"$(setting "$1")\", want \"$2\""
  }
  expect hot_corners.top_left.action "shell:panel-toggle launcher"
  expect hot_corners.top_left.delay_ms 250
  expect hot_corners.top_left.enabled true
  expect hot_corners.bottom_right.action overview-toggle
  expect hot_corners.bottom_right.enabled true
  run_client settings-state | grep "^hot_corners" > "$OUT/settings.txt"
  step "handed over: $(tr "\n" " " < "$OUT/settings.txt")"

  run_client settings-set hot_corners.top_left.action "spawn:foot"
  expect hot_corners.top_left.action spawn:foot
  kill "$(pgrep -f "^$NOCTALIA\$" | head -1)"
  sleep 1
  "$NOCTALIA" >> "$OUT/noctalia.log" 2>&1 &
  for _ in $(seq 100); do "$NOCTALIA" msg settings-close > /dev/null 2>&1 && break; sleep 0.1; done
  sleep 1 # real time: a second handover, if the shell wrongly made one, would land by now
  [[ $(setting hot_corners.top_left.action) == spawn:foot ]] \
    || fail "a restarted shell overwrote the compositor corner: $(setting hot_corners.top_left.action)"
  step "a restart left the compositor corners alone"
'
cat "$OUT/steps.txt"
echo "PASS; artifacts: $OUT"
