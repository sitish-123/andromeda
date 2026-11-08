#!/usr/bin/env bash
# Walks the first-login setup wizard with real pointer clicks: Welcome -> Appearance -> Keyboard -> [Time Zone] -> [Phone,
# Set Up Later] -> [Default Apps] -> Search -> Back to Keyboard -> Search -> Software update (the check fails) -> Done.
# Checks the page after each click from the shell log and that Done writes the completion marker.
# Needs NOCTALIA (absolute). UI_SCALE=1.25 runs it at that scale.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${OUT:-/tmp/andromeda-e2e/setup_wizard_flow}
UI_SCALE=${UI_SCALE:-1}
source "$(dirname "$0")/lib.sh"
export POINTER=${POINTER:-$ROOT/compositor/build-debug/tests/pointer-client} UI_SCALE

boot_headless 1
sed -i 's/setup_wizard_enabled = false/setup_wizard_enabled = true/' "$RUNTIME/home/.config/noctalia/config.toml"
printf '\n[accessibility]\nui_scale = %s\n' "$UI_SCALE" >> "$RUNTIME/home/.config/noctalia/config.toml"
# A software update check that always fails, so the run needs no network and proves a failed step does not block.
mkdir -p "$RUNTIME/bin"
printf '#!/bin/sh\necho "==> ERROR: Cannot fetch updates" >&2\nexit 1\n' > "$RUNTIME/bin/checkupdates"
chmod +x "$RUNTIME/bin/checkupdates"
export PATH=$RUNTIME/bin:$PATH

with_noctalia '
  fail() { echo "FAIL: $*" >&2; tail -n 20 "$OUT/noctalia.log" >&2; exit 1; }
  # wait_page NAME: the wizard logged "page NAME" as its newest page line within 5 s.
  wait_page() {
    for _ in $(seq 50); do
      [[ $(grep -o "page [a-z]*" "$OUT/noctalia.log" | tail -n 1) == "page $1" ]] && return 0
      sleep 0.1
    done
    fail "expected page $1, log has: $(grep -o "page [a-z]*" "$OUT/noctalia.log" | tail -n 1)"
  }
  click() { "$POINTER" 1280 720 move "$1" "$footer" click 272 > /dev/null; sleep 0.4; }
  # Offsets in points x 100, so they follow UI_SCALE: primary is at least 96 wide, Set Up Later sits 8 left of it.
  pct=$(awk -v s="$UI_SCALE" "BEGIN { print int(s * 100) }")
  lead() { click $((left + 20 * pct / 100)); }
  primary() { click $((right - 40 * pct / 100)); }
  later() { click $((right - 160 * pct / 100)); }

  wait_page welcome
  # The page line comes before the first frame; a click before the panel maps is lost.
  for _ in $(seq 100); do grep -q "panel \"setup-wizard\" open to first frame" "$OUT/noctalia.log" && break; sleep 0.1; done
  sleep 1
  grim "$OUT/welcome.png"
  # The 640 x 560 sheet sits centred in the panel layer (the rest is its shadow); footer buttons are 24 in, 32 high.
  read -r left right footer < <("$ANDROMEDA" layers --json | jq -r --argjson s "$UI_SCALE" \
    ".[] | select(.namespace == \"noctalia-panel\") | (.x + (.w - 640 * \$s) / 2) as \$l | (.y + (.h + 560 * \$s) / 2) as \$b |
     \"\(\$l + 24 * \$s | floor) \(\$l + 640 * \$s - 24 * \$s | floor) \(\$b - 40 * \$s | floor)\"")
  [[ -n $footer ]] || fail "no noctalia-panel layer"
  # last_page: the newest page line. reach CLICK TARGET: after the click, step over the pages that show only with
  # their backend (Time Zone: timedated; Phone: the Link unit; Default Apps: two apps per role) until TARGET, 5 s each.
  last_page() { grep -o "page [a-z]*" "$OUT/noctalia.log" | tail -n 1 | cut -d" " -f2; }
  reach() {
    local click=$1 target=$2 before page
    before=$(grep -c "page " "$OUT/noctalia.log")
    $click
    for _ in $(seq 50); do
      page=$(last_page)
      if (( $(grep -c "page " "$OUT/noctalia.log") > before )); then
        [[ $page == "$target" ]] && return 0
        case $page in
          timezone | apps) before=$(grep -c "page " "$OUT/noctalia.log"); $click ;;
          phone) before=$(grep -c "page " "$OUT/noctalia.log"); [[ $click == lead ]] && lead || later ;;
        esac
      fi
      sleep 0.1
    done
    fail "expected page $target, log has: page $page"
  }
  primary; wait_page appearance
  reach primary layout
  reach primary keyboard
  reach primary search
  reach lead keyboard
  reach primary search
  primary; wait_page update
  sleep 1
  grim "$OUT/update.png"
  primary; wait_page done
  ! find "$HOME" -name .setup-complete | grep -q . || fail "marker written before Done"
  primary
  for _ in $(seq 50); do grep -q "setup complete" "$OUT/noctalia.log" && break; sleep 0.1; done
  grep -q "setup complete" "$OUT/noctalia.log" || fail "Done did not finish the wizard"
  find "$HOME" -name .setup-complete | grep -q . || fail "no completion marker"
  echo PASS
'
