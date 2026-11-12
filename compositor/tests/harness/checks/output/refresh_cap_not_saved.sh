#!/usr/bin/env bash
# harness: outputs=1
# An output-management apply saves only what the request changed. A scale change made while the refresh cap holds
# the panel at 60 Hz must not save that capped mode as the user's pick: after the reload the cap still limits the
# output, so it returns to its fastest mode once the cap ends. Runs one private instance on the user bus, where the
# cap is set.
set -euo pipefail

readonly OUTPUT_MANAGEMENT=${ANDROMEDA_OUTPUT_MANAGEMENT_CLIENT:-./build-debug/tests/output-management-client}
readonly USER_BUS=/run/user/$(id -u)/bus
if [[ ! -S $USER_BUS ]] || ! python3 -c 'import dbus' 2> /dev/null; then
  echo "SKIP: needs the user bus and python3-dbus"
  exit 0
fi

RUNTIME_DIR=$(mktemp -d /tmp/rcs.XXXXXXXX)
SERVER_PID=
# The compositor stops before the holder: a released cap would commit a listed mode, which headless cannot.
cleanup() {
  if [[ -n $SERVER_PID ]] && kill -0 "$SERVER_PID" 2> /dev/null; then
    kill -KILL "$SERVER_PID" 2> /dev/null || true
    wait "$SERVER_PID" 2> /dev/null || true
  fi
  [[ -n ${HOLDER_PID:-} ]] && kill "$HOLDER_PID" 2> /dev/null || true
  rm -rf "$RUNTIME_DIR"
}
trap cleanup EXIT

readonly CONFIG=$RUNTIME_DIR/config.toml
readonly SAVED=$RUNTIME_DIR/displays.toml
readonly SOCKET=$RUNTIME_DIR/andromeda-wayland-0.sock
readonly BUS_NAME=org.andromeda.Compositor1.CapSaved$$
ln -s "$USER_BUS" "$RUNTIME_DIR/bus"
cat > "$CONFIG" << 'EOF'
[include.optional]
files = ["displays.toml"]

[general]
xwayland = false
autostart = []
EOF

env -u WAYLAND_DISPLAY -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS \
  XDG_RUNTIME_DIR="$RUNTIME_DIR" \
  ANDROMEDA_COMPOSITOR_BUS_NAME="$BUS_NAME" \
  WLR_BACKENDS=headless \
  WLR_LIBINPUT_NO_DEVICES=1 \
  WLR_HEADLESS_OUTPUTS=1 \
  "$ANDROMEDA" -c "$CONFIG" > "$RUNTIME_DIR/compositor.log" 2>&1 &
SERVER_PID=$!
for _ in $(seq 40); do
  [[ -S $SOCKET ]] && busctl --address="unix:path=$USER_BUS" status "$BUS_NAME" > /dev/null 2>&1 && break
  sleep 0.25
done
if [[ ! -S $SOCKET ]] || ! busctl --address="unix:path=$USER_BUS" status "$BUS_NAME" > /dev/null 2>&1; then
  echo "the private compositor never exposed its IPC socket and bus name"
  sed 's/^/  | /' "$RUNTIME_DIR/compositor.log"
  exit 1
fi
private() {
  ANDROMEDA_SOCKET=$SOCKET XDG_RUNTIME_DIR=$RUNTIME_DIR WAYLAND_DISPLAY=wayland-0 "$@"
}
limits() { private "$ANDROMEDA" outputs --json | jq -r '.[0].refresh_cap.limits'; }

# The holder keeps its bus connection: the cap ends when its owner leaves the bus.
coproc HOLDER {
  DBUS_SESSION_BUS_ADDRESS="unix:path=$USER_BUS" \
  python3 -u -c '
import sys, dbus
apps = dbus.Interface(dbus.SessionBus().get_object(sys.argv[1], "/org/andromeda/Compositor1/Apps"),
                      "org.andromeda.Compositor1.Apps")
apps.SetRefreshCap(dbus.UInt32(60))
print("set", flush=True)
sys.stdin.read()' "$BUS_NAME"
}
read -r line <&"${HOLDER[0]}"
[[ $line == set ]] || { echo "SetRefreshCap failed"; exit 1; }
private "$ANDROMEDA" output-test-modes HEADLESS-1 1280x720@144 1280x720@60.02 > /dev/null
[[ $(limits) == true ]] || { echo "the cap does not limit HEADLESS-1 before the apply"; exit 1; }

OUTPUT_MANAGEMENT_SCALE=2 private "$OUTPUT_MANAGEMENT" apply enable HEADLESS-1 > /dev/null
for _ in $(seq 40); do
  grep -qs '^scale = 2' "$SAVED" && break
  sleep 0.1
done
grep -qs '^scale = 2' "$SAVED" || { echo "the scale change was not saved:"; sed 's/^/  | /' "$SAVED"; exit 1; }
if grep -q '^mode' "$SAVED"; then
  echo "a scale change saved the capped mode as the user's pick:"
  sed 's/^/  | /' "$SAVED"
  exit 1
fi

private "$ANDROMEDA" msg config-reload > /dev/null
[[ $(limits) == true ]] || { echo "after the reload the cap no longer limits HEADLESS-1"; exit 1; }

echo "a scale change under the refresh cap saves only the scale; the cap still governs the mode"
