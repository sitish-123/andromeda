#!/usr/bin/env bash
# Boots a headless Umbriel with two outputs. Drives OutputManagement through apply, disable, and revert against it
# (output-apply.txt), then runs the Noctalia fork, opens Settings > Displays, and screenshots it (displays.png).
# Artifacts go to $OUT (default ./artifacts/displays). Exits non-zero if any step fails.
set -euo pipefail

UMBRIEL=${UMBRIEL:-$HOME/src/umbriel/build-debug/umbriel}
NOCTALIA=${NOCTALIA:-$HOME/src/noctalia/build-debug/noctalia}
OUT=${OUT:-$(pwd)/artifacts/displays}
mkdir -p "$OUT"
RUNTIME=$(mktemp -d /tmp/dsk.XXXX)
trap 'kill $(jobs -p) 2>/dev/null; wait 2>/dev/null; rm -rf "$RUNTIME"' EXIT

printf '[general]\nautostart = []\nshow_cheatsheet = false\n' > "$RUNTIME/umbriel.toml"
# No service directories, so the private bus never activates a keyring prompt or dconf.
cat > "$RUNTIME/bus.conf" <<'CONF'
<busconfig>
  <type>session</type>
  <listen>unix:tmpdir=/tmp</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow user="*"/>
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
CONF

SRC=$HOME/src/noctalia
BUILD=$SRC/build-debug
gcc -c -o "$RUNTIME/proto.o" "$BUILD/wlr-output-management-unstable-v1-client-protocol.c"
g++ -std=c++23 -I"$SRC/src" -I"$BUILD" -o "$RUNTIME/output-apply" \
  "$(dirname "$0")/output_apply.cpp" "$SRC/src/wayland/output_management.cpp" "$RUNTIME/proto.o" -lwayland-client
env -u WAYLAND_DISPLAY -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS XDG_RUNTIME_DIR="$RUNTIME" \
  WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_HEADLESS_OUTPUTS=2 \
  "$UMBRIEL" -c "$RUNTIME/umbriel.toml" > "$OUT/umbriel.log" 2>&1 &
for _ in $(seq 100); do [[ -S $RUNTIME/wayland-0 ]] && break; sleep 0.05; done
[[ -S $RUNTIME/wayland-0 ]] || { echo "umbriel did not start" >&2; exit 1; }

mkdir -p "$RUNTIME/home"
# A private HOME keeps Noctalia's theme templates and gsettings writes out of the real user config.
run() {
  env -u DISPLAY -u XDG_CONFIG_HOME -u XDG_DATA_HOME -u XDG_STATE_HOME -u XDG_CACHE_HOME HOME="$RUNTIME/home" \
    XDG_RUNTIME_DIR="$RUNTIME" WAYLAND_DISPLAY=wayland-0 UMBRIEL_SOCKET="$RUNTIME/umbriel-wayland-0.sock" "$@"
}

run "$UMBRIEL" outputs > "$OUT/outputs-before.txt"
run "$RUNTIME/output-apply" | tee "$OUT/output-apply.txt"
run "$UMBRIEL" outputs > "$OUT/outputs-after.txt"
run dbus-run-session --config-file="$RUNTIME/bus.conf" -- bash -c '
  "$1" > "$2/noctalia.log" 2>&1 &
  for _ in $(seq 100); do "$1" msg settings-open displays > /dev/null 2>&1 && break; sleep 0.1; done
  sleep 2
  grim "$2/displays.png"
  kill %1
  wait
' _ "$NOCTALIA" "$OUT"
[[ -s $OUT/displays.png ]] || { echo "no screenshot" >&2; exit 1; }
echo "artifacts: $OUT"
