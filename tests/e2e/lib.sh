# Sourced by the E2E scripts: a contained headless Umbriel session with the Noctalia fork running inside it.
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
UMBRIEL=${UMBRIEL:-$ROOT/compositor/build-debug/umbriel}
NOCTALIA=${NOCTALIA:-$ROOT/shell/build-debug/noctalia}
DESKTOP_CLIENT=${DESKTOP_CLIENT:-$ROOT/compositor/build-debug/tests/desktop-client}
RUNTIME=$(mktemp -d /tmp/dsk.XXXX)
trap 'kill $(jobs -p) 2>/dev/null || true; wait 2>/dev/null; rm -rf "$RUNTIME"' EXIT

# boot_headless OUTPUTS: start Umbriel with that many headless outputs; its log goes to $OUT/umbriel.log.
boot_headless() {
  mkdir -p "$OUT" "$RUNTIME/home/.config/noctalia"
  printf '[plugins]\nauto_update = "none"\n' > "$RUNTIME/home/.config/noctalia/config.toml"
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
  env -u WAYLAND_DISPLAY -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS XDG_RUNTIME_DIR="$RUNTIME" \
    WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_HEADLESS_OUTPUTS="$1" \
    "$UMBRIEL" -c "$RUNTIME/umbriel.toml" > "$OUT/umbriel.log" 2>&1 &
  for _ in $(seq 100); do [[ -S $RUNTIME/wayland-0 ]] && break; sleep 0.05; done
  [[ -S $RUNTIME/wayland-0 ]] || { echo "umbriel did not start" >&2; exit 1; }
}

# run CMD...: run inside the session. A private HOME keeps Noctalia's theme templates and gsettings writes out of the
# real user config.
run() {
  env -u DISPLAY -u XDG_CONFIG_HOME -u XDG_DATA_HOME -u XDG_STATE_HOME -u XDG_CACHE_HOME HOME="$RUNTIME/home" \
    XDG_RUNTIME_DIR="$RUNTIME" WAYLAND_DISPLAY=wayland-0 UMBRIEL_SOCKET="$RUNTIME/umbriel-wayland-0.sock" "$@"
}

# with_noctalia SCRIPT: run a bash script while Noctalia runs on a private bus, used as both session and system bus
# so the test shell never reaches the real BlueZ, NetworkManager, or logind. The script sees $NOCTALIA, $OUT,
# $UMBRIEL, $DESKTOP_CLIENT, and $RUNTIME, and Noctalia is answering IPC when it starts.
with_noctalia() {
  run env NOCTALIA="$NOCTALIA" OUT="$OUT" UMBRIEL="$UMBRIEL" DESKTOP_CLIENT="$DESKTOP_CLIENT" RUNTIME="$RUNTIME" \
    dbus-run-session --config-file="$RUNTIME/bus.conf" -- bash -c '
      set -euo pipefail
      export DBUS_SYSTEM_BUS_ADDRESS=$DBUS_SESSION_BUS_ADDRESS
      "$NOCTALIA" > "$OUT/noctalia.log" 2>&1 &
      for _ in $(seq 100); do "$NOCTALIA" msg settings-close > /dev/null 2>&1 && break; sleep 0.1; done
      eval "$1"
      kill $(jobs -p) 2>/dev/null || true
      wait
    ' _ "$1"
}
