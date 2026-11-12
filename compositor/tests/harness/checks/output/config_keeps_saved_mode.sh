#!/usr/bin/env bash
# harness: outputs=1
# A config.toml [output] table that sets only some keys overrides only those keys: a mode applied through
# wlr-output-management is still written to displays.toml, and the next session starts with that mode and the
# config.toml scale. Runs on one private instance with the harness's containment.
set -euo pipefail

readonly OUTPUT_MANAGEMENT=${ANDROMEDA_OUTPUT_MANAGEMENT_CLIENT:-./build-debug/tests/output-management-client}

RUNTIME_DIR=$(mktemp -d /tmp/ckm.XXXXXXXX)
SERVER_PID=
stop_server() {
  if [[ -n $SERVER_PID ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill -KILL "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
  SERVER_PID=
}
cleanup() {
  stop_server
  rm -rf "$RUNTIME_DIR"
}
trap cleanup EXIT

readonly CONFIG=$RUNTIME_DIR/config.toml
readonly SAVED=$RUNTIME_DIR/displays.toml
readonly SOCKET=$RUNTIME_DIR/andromeda-wayland-0.sock

write_config() {
  cat > "$CONFIG" << EOF
[include.optional]
files = ["displays.toml"]

[general]
xwayland = false
autostart = []

[output.HEADLESS-1]
scale = $1
EOF
}

start_server() {
  rm -f "$SOCKET"
  env -u WAYLAND_DISPLAY -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS \
    XDG_RUNTIME_DIR="$RUNTIME_DIR" \
    WLR_BACKENDS=headless \
    WLR_LIBINPUT_NO_DEVICES=1 \
    WLR_HEADLESS_OUTPUTS=1 \
    "$ANDROMEDA" -c "$CONFIG" >> "$RUNTIME_DIR/compositor.log" 2>&1 &
  SERVER_PID=$!
  for _ in $(seq 40); do
    [[ -S $SOCKET ]] && return 0
    sleep 0.25
  done
  echo "the private compositor never exposed its IPC socket"
  sed 's/^/  | /' "$RUNTIME_DIR/compositor.log"
  exit 1
}

private() {
  ANDROMEDA_SOCKET=$SOCKET XDG_RUNTIME_DIR=$RUNTIME_DIR WAYLAND_DISPLAY=wayland-0 "$@"
}

state() {
  private "$ANDROMEDA" outputs --json \
    | jq -r '.[] | select(.name == "HEADLESS-1")
        | (.modes[] | select(.current) | "\(.width)x\(.height)") + " scale \(.scale)"'
}

write_config 2
start_server
OUTPUT_MANAGEMENT_MODE=1600x900@60000 private "$OUTPUT_MANAGEMENT" apply enable HEADLESS-1 > /dev/null
for _ in $(seq 40); do
  grep -qsE '^mode = ["'\'']1600x900' "$SAVED" && break
  sleep 0.1
done
if ! grep -qsE '^mode = ["'\'']1600x900' "$SAVED"; then
  echo "a mode applied to HEADLESS-1 was not written to displays.toml while config.toml sets only its scale:"
  sed 's/^/  | /' "$SAVED" 2> /dev/null || echo "  | (no displays.toml)"
  exit 1
fi
stop_server

write_config 1.5
start_server
actual=
for _ in $(seq 40); do
  actual=$(state)
  [[ $actual == "1600x900 scale 1.5" ]] && break
  sleep 0.1
done
if [[ $actual != "1600x900 scale 1.5" ]]; then
  echo "the next session started HEADLESS-1 at '$actual', expected the saved mode with the config.toml scale"
  echo "  '1600x900 scale 1.5'"
  exit 1
fi

echo "config.toml overrides only its own [output] keys; the saved mode survives the next session"
