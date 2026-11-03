#!/usr/bin/env bash
# Plugin lock widgets draw in `noctalia lock-helper`, not in the shell (L5b). While locked the helper starts and its
# frame shows (slot.png: a red box at 200,200), and a shader slot reaches the shell as a dmabuf (a green box at
# 400,200). Killing the helper leaves the session locked, the shell starts a new
# helper, and the password field still takes input (a wrong password reaches PAM). Writes slot.png, restarted.png and
# noctalia.log to $OUT (default ./artifacts/lock-helper-crash).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/lock-helper-crash}
source "$(dirname "$0")/lib.sh"
rm -rf "$OUT"
boot_headless 1
mkdir -p "$RUNTIME/home/.local/share/noctalia/plugins"
cp -r "$ROOT/shell/tests/lock_helper/me-lockbox" "$RUNTIME/home/.local/share/noctalia/plugins/"
cat > "$RUNTIME/home/.config/noctalia/config.toml" <<'TOML'
[plugins]
auto_update = "none"
enabled = ["me/lockbox"]

[shell]
setup_wizard_enabled = false

[lockscreen]
layout = "custom"

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

with_noctalia '
  fail() { echo "FAIL: $*" >&2; exit 1; }
  msg() { "$NOCTALIA" msg "$@" 2>&1 || true; }
  locked() { msg status | grep -q "\"locked\": true"; }
  wait_for() { local what=$1; shift; for _ in $(seq 100); do "$@" && return; sleep 0.1; done; grim "$OUT/fail.png" || true; fail "$what"; }
  helpers() { grep -c "started lock helper pid" "$OUT/noctalia.log" | grep -qx "$1"; }
  green() { grim -g "400,200 1x1" -t ppm - | tail -c 3 | od -An -tu1 | awk "{ exit !(\$1 < 60 && \$2 > 200) }"; }
  red() { grim -g "200,200 1x1" -t ppm - | tail -c 3 | od -An -tu1 | awk "{ exit !(\$1 > 200 && \$2 < 60) }"; }

  wtype -s 3600000 &
  sleep 0.3
  [[ $(msg session lock) == ok ]] || fail "lock refused"
  wait_for "did not lock" locked
  wait_for "the lock helper did not start" helpers 1
  wait_for "the helper frame did not show" red
  wait_for "the shader frame did not show" green
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
  echo "PASS"
'
