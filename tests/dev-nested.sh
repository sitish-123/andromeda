#!/usr/bin/env bash
# A test desktop in a window: compositor/build-live + shell/build-f7 with copies of the user's config, a private
# session bus (no clash with the main shell's bus names), the real system bus (Wi-Fi, Bluetooth, battery) and the real
# PipeWire (audio). No backlight control: the nested output is a window.
# Close the window or press Ctrl+C to end it; rerun after a compositor rebuild.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
ANDROMEDA=${ANDROMEDA:-$ROOT/compositor/build-live/andromeda}
NOCTALIA=${NOCTALIA:-$ROOT/shell/build-f7/noctalia}
PARENT=${WAYLAND_DISPLAY:?run this inside a Wayland session}
[[ $PARENT == /* ]] || PARENT=$XDG_RUNTIME_DIR/$PARENT
RUNTIME=$(mktemp -d /tmp/dsk-nested.XXXX)
trap 'kill $(jobs -p) 2>/dev/null || true; wait 2>/dev/null; rm -rf "$RUNTIME"' EXIT

H=$RUNTIME/home
mkdir -p "$H/.config" "$H/.local/state"
cp -r ~/.config/noctalia "$H/.config/" 2>/dev/null || true
cp -r ~/.local/state/noctalia "$H/.local/state/" 2>/dev/null || true
ln -s ~/Pictures "$H/Pictures"
# The user's compositor config without its autostart (the shell starts below) and without the cheatsheet on each start.
sed -e '/^autostart *=/d' -e '/^show_cheatsheet *=/d' -e '/^\[general\]/a show_cheatsheet = false' \
  ~/.config/andromeda/config.toml > "$RUNTIME/andromeda.toml"

env -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS XDG_RUNTIME_DIR="$RUNTIME" WAYLAND_DISPLAY="$PARENT" WLR_BACKENDS=wayland \
  "$ANDROMEDA" -c "$RUNTIME/andromeda.toml" > "$RUNTIME/andromeda.log" 2>&1 &
for _ in $(seq 100); do [[ -S $RUNTIME/wayland-0 ]] && break; sleep 0.05; done
[[ -S $RUNTIME/wayland-0 ]] || { echo "andromeda did not start:" >&2; tail -n 20 "$RUNTIME/andromeda.log" >&2; exit 1; }

env -u DISPLAY -u XDG_CONFIG_HOME -u XDG_DATA_HOME -u XDG_STATE_HOME -u XDG_CACHE_HOME HOME="$H" \
  XDG_RUNTIME_DIR="$RUNTIME" WAYLAND_DISPLAY=wayland-0 ANDROMEDA_SOCKET="$RUNTIME/andromeda-wayland-0.sock" \
  ANDROMEDA_DEV_TOKENS="$ROOT/protocol/design-tokens.toml" NOCTALIA="$NOCTALIA" LOG="$RUNTIME/noctalia.log" \
  PIPEWIRE_RUNTIME_DIR="$XDG_RUNTIME_DIR" PULSE_SERVER="unix:$XDG_RUNTIME_DIR/pulse/native" \
  dbus-run-session -- bash -c '"$NOCTALIA" > "$LOG" 2>&1 & sleep 2; foot & wait' &
echo "nested desktop: logs in $RUNTIME (andromeda.log); Ctrl+C ends it"
wait %1
