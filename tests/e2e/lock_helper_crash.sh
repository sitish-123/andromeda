#!/usr/bin/env bash
# Plugin lock widgets draw in `noctalia lock-helper`, not in the shell (L5b). While locked the helper starts and its
# frame shows (slot.png: a red box at 200,200), and a shader slot reaches the shell as a dmabuf (a green box at
# 400,200). A shader lock wallpaper (lockscreen.wallpaper = a .frag file) fills the background (green at
# 40,600). Killing the helper leaves the session locked, the shell starts a new helper, and the password field
# still takes input (a wrong password reaches PAM). P0: with the lock process (`noctalia lock`) running, the shell
# delegates the lock; the lock process runs the helper (the shell starts none), and a face match unlocks and ends the
# helper. P2: the shell dies while locked; the lock process (`noctalia
# lock`) locks again and runs its own helper with the same frames, and a killed helper restarts there too. Writes
# p0.png, slot.png, restarted.png, lock-slot.png, lock-restarted.png, noctalia*.log, lock*.log, mock.log and calls.txt to $OUT
# (default ./artifacts/lock-helper-crash).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/lock-helper-crash}
source "$(dirname "$0")/lib.sh"
rm -rf "$OUT"
boot_headless 1
mkdir -p "$RUNTIME/home/.local/share/andromeda/shell/plugins"
cp -r "$ROOT/shell/tests/lock_helper/me-lockbox" "$RUNTIME/home/.local/share/andromeda/shell/plugins/"
cat > "$RUNTIME/home/.config/andromeda/shell/config.toml" <<'TOML'
[plugins]
auto_update = "none"
enabled = ["me/lockbox"]

[shell]
setup_wizard_enabled = false

[lockscreen]
layout = "custom"
wallpaper = "~/green.frag"
face = true
face_on_request = false

[lockscreen_widgets]
enabled = true
widget_order = ["box", "sh"]

[lockscreen_widgets.widget.box]
type = "me/lockbox:box"
cx = 200.0
cy = 200.0
box_width = 80.0
box_height = 80.0

[lockscreen_widgets.widget.sh]
type = "shader"
cx = 400.0
cy = 200.0
box_width = 80.0
box_height = 80.0

[lockscreen_widgets.widget.sh.settings]
path = "~/green.frag"
TOML
echo 'void mainImage(out vec4 fragColor, in vec2 fragCoord) { fragColor = vec4(0.0, 1.0, 0.0, 1.0); }' \
  > "$RUNTIME/home/green.frag"

MOCK="$(cd "$(dirname "$0")" && pwd)/mock_gaze.py"
with_noctalia '
  fail() { echo "FAIL: $*" >&2; exit 1; }
  msg() { "$NOCTALIA" msg "$@" 2>&1 || true; }
  locked() { msg status | grep -q "\"locked\": true"; }
  wait_for() { local what=$1; shift; for _ in $(seq 100); do "$@" && return; sleep 0.1; done; grim "$OUT/fail.png" || true; fail "$what"; }
  helpers() { grep -c "started lock helper pid" "$OUT/noctalia.log" | grep -qx "$1"; }
  green() { grim -g "400,200 1x1" -t ppm - | tail -c 3 | od -An -tu1 | awk "{ exit !(\$1 < 60 && \$2 > 200) }"; }
  backdrop() { grim -g "40,600 1x1" -t ppm - | tail -c 3 | od -An -tu1 | awk "{ exit !(\$2 > \$1 + 30 && \$2 > \$3 + 30) }"; }
  red() { grim -g "200,200 1x1" -t ppm - | tail -c 3 | od -An -tu1 | awk "{ exit !(\$1 > 200 && \$2 < 60) }"; }

  unlocked() { msg status | grep -q "\"locked\": false"; }
  gaze() { busctl --address="$DBUS_SYSTEM_BUS_ADDRESS" call com.gundulabs.Gaze /com/gundulabs/Gaze dsk.test.Gaze "$@" \
    > /dev/null; }
  verifying() { grep -q "^VerifyStartFor" "$OUT/calls.txt" 2> /dev/null; }
  served() { busctl --user status org.andromeda.Lock1 > /dev/null 2>&1; }
  shell_up() { for _ in $(seq 100); do "$NOCTALIA" msg settings-close > /dev/null 2>&1 && return; sleep 0.1; done; }
  dead() { [[ ! -e /proc/$1 ]] || grep -q "^State:.*Z" "/proc/$1/status"; }

  kill %1; wait %1 2> /dev/null || true
  python3 '"$MOCK"' "$OUT/calls.txt" >> "$OUT/mock.log" 2>&1 &
  wait_for "the mock did not start" busctl --address="$DBUS_SYSTEM_BUS_ADDRESS" status com.gundulabs.Gaze
  wtype -s 3600000 &
  sleep 0.3
  "$NOCTALIA" lock > "$OUT/lock-p0.log" 2>&1 &
  lockpid=$!
  wait_for "P0: org.andromeda.Lock1 is not served" served
  "$NOCTALIA" > "$OUT/noctalia-p0.log" 2>&1 &
  shell=$!
  shell_up
  wait_for "P0: the shell does not delegate" grep -q "delegates every lock" "$OUT/noctalia-p0.log"
  [[ $(msg session lock) == ok ]] || fail "P0: lock refused"
  wait_for "P0: did not lock" locked
  wait_for "P0: the lock process did not start a helper" grep -q "started lock helper pid" "$OUT/lock-p0.log"
  wait_for "P0: the helper frame did not show" red
  wait_for "P0: the shader frame did not show" green
  wait_for "P0: the shader wallpaper did not show" backdrop
  grim "$OUT/p0.png"
  ! grep -q "started lock helper" "$OUT/noctalia-p0.log" || fail "P0: the delegating shell started a helper"
  pid=$(grep -o "started lock helper pid [0-9]*" "$OUT/lock-p0.log" | tail -1 | grep -o "[0-9]*$")
  wait_for "P0: face verify did not start" verifying
  gaze Finish sssb verify-match usable usable true
  wait_for "P0: a face match did not unlock" unlocked
  wait_for "P0: the helper outlived the lock" dead "$pid"
  kill "$shell" "$lockpid"; wait "$shell" "$lockpid" 2> /dev/null || true
  echo "P0 pass: a delegated lock runs the helper in the lock process"

  "$NOCTALIA" > "$OUT/noctalia.log" 2>&1 &
  shell=$!
  shell_up
  [[ $(msg session lock) == ok ]] || fail "lock refused"
  wait_for "did not lock" locked
  wait_for "the lock helper did not start" helpers 1
  wait_for "the helper frame did not show" red
  wait_for "the shader frame did not show" green
  wait_for "the shader wallpaper did not show" backdrop
  grep -q "shader slot sends dmabufs" "$OUT/noctalia.log" || fail "the shader slot did not send dmabufs"
  ! grep -q "dmabuf import failed" "$OUT/noctalia.log" || fail "the shell did not import the dmabuf"
  grim "$OUT/slot.png"

  pid=$(grep -o "started lock helper pid [0-9]*" "$OUT/noctalia.log" | tail -1 | grep -o "[0-9]*$")
  kill -9 "$pid"
  wait_for "the shell did not restart the helper" helpers 2
  locked || fail "killing the helper unlocked the session"
  wait_for "the restarted helper frame did not show" red
  grim "$OUT/restarted.png"

  wtype wrong-password
  wtype -k Return
  wait_for "the password field took no input" grep -q "authentication failed" "$OUT/noctalia.log"
  locked || fail "the session is not locked after a wrong password"
  echo "P1 pass: the shell runs the helper"

  lock_helpers() { grep -c "started lock helper pid" "$OUT/lock.log" | grep -qx "$1"; }
  relocked() { [[ $(grep -c "session is locked" "$OUT/andromeda.log") -ge 3 ]]; }
  kill -9 "$shell"; wait "$shell" 2> /dev/null || true
  "$NOCTALIA" lock > "$OUT/lock.log" 2>&1 &
  wait_for "P2: the lock process did not lock again" relocked
  wait_for "P2: the lock process did not start a helper" lock_helpers 1
  wait_for "P2: the helper frame did not show" red
  wait_for "P2: the shader frame did not show" green
  wait_for "P2: the shader wallpaper did not show" backdrop
  grim "$OUT/lock-slot.png"
  pid=$(grep -o "started lock helper pid [0-9]*" "$OUT/lock.log" | tail -1 | grep -o "[0-9]*$")
  kill -9 "$pid"
  wait_for "P2: the lock process did not restart the helper" lock_helpers 2
  wait_for "P2: the restarted helper frame did not show" red
  grim "$OUT/lock-restarted.png"
  echo "P2 pass: the lock process runs the helper"
  echo "PASS"
'
