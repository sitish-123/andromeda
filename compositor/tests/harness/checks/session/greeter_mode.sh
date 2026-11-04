#!/usr/bin/env bash
# `andromeda --greeter -s CMD` runs a login screen session: each toplevel is fullscreen on its own output, privileged globals are
# hidden, settings are read-only, no IPC socket exists, and the compositor exits within 3 s of the greeter.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly RT=$(mktemp -d /tmp/grt.XXXX)
trap 'kill "$server_pid" 2> /dev/null || true; rm -rf "$RT"' EXIT
readonly RESULT="$RT/result.log"
readonly CLIENT_LOG="$RT/client.log"
printf '[general]\nautostart = []\n\n[output.HEADLESS-1]\nposition = [0, 0]\n\n[output.HEADLESS-2]\nmode = "1024x768"\nposition = [1280, 0]\n' > "$RT/greeter.toml"

cat > "$RT/greeter.sh" << SH
#!/usr/bin/env bash
g() { "$ANDROMEDA_GLOBAL_CLIENT" "\$@" > /dev/null 2>&1 && echo "ok \$*" || echo "bad \$*"; }
g dsk_settings_manager_v1 present
g zwlr_screencopy_manager_v1 absent
g dsk_output_manager_v1 absent
if "$ANDROMEDA_DESKTOP_CLIENT" settings-set appearance.border_width 3 > /dev/null 2>&1; then
  echo "bad settings-set accepted"
else
  echo "ok settings-set refused"
fi
echo started
LOG_CONFIGURES=1 FOLLOW_CONFIGURES=1 timeout 2 "$ANDROMEDA_UNMAP_CLIENT" greeter-a 400 300 >> "$CLIENT_LOG" 2>&1 &
for _ in \$(seq 100); do
  grep -q '^configured-state=.* fullscreen' "$CLIENT_LOG" 2> /dev/null && break
  sleep 0.02
done
LOG_CONFIGURES=1 FOLLOW_CONFIGURES=1 timeout 2 "$ANDROMEDA_UNMAP_CLIENT" greeter-b 400 300 >> "$CLIENT_LOG" 2>&1 &
wait
echo client-exited
SH
chmod +x "$RT/greeter.sh"

env -u WAYLAND_DISPLAY -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS XDG_RUNTIME_DIR="$RT" WLR_BACKENDS=headless \
  WLR_LIBINPUT_NO_DEVICES=1 WLR_HEADLESS_OUTPUTS=2 WAYLAND_DISPLAY=wayland-0 \
  "$ANDROMEDA" --greeter -c "$RT/greeter.toml" -s "$RT/greeter.sh >> $RESULT 2>&1" > "$RT/compositor.log" 2>&1 &
server_pid=$!

await_lines "$RESULT" started 1
! grep -q '^bad' "$RESULT" || { cat "$RESULT"; exit 1; }
[[ $(grep -c '^ok' "$RESULT") == 4 ]] || { cat "$RESULT"; exit 1; }
await_lines "$CLIENT_LOG" 'configured-state=1280x720 fullscreen' 1
await_lines "$CLIENT_LOG" 'configured-state=1024x768 fullscreen' 1
await_lines "$RESULT" client-exited 1
ls "$RT" | grep -q 'andromeda-wayland.*sock' && { echo "an IPC socket exists in greeter mode"; exit 1; }
for _ in $(seq 60); do
  kill -0 "$server_pid" 2> /dev/null || break
  sleep 0.05
done
if kill -0 "$server_pid" 2> /dev/null; then
  echo "the compositor outlived the greeter"
  tail -n 20 "$RT/compositor.log"
  exit 1
fi
