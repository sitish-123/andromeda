#!/usr/bin/env bash
# Static lock surfaces must stay idle while real output resizes still configure them.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly LOCK_LOG="$ANDROMEDA_RUNTIME_DIR/lock-client.log"
readonly PROTOCOL_LOG="$ANDROMEDA_RUNTIME_DIR/lock-protocol.log"
readonly CONFIGURE_PATTERN='.*ext_session_lock_surface_v1.*\.configure('
mkfifo "$ANDROMEDA_RUNTIME_DIR/lock-control"
exec {lock_fd}<> "$ANDROMEDA_RUNTIME_DIR/lock-control"
WAYLAND_DEBUG=client "$ANDROMEDA_LOCK_CLIENT" <&"$lock_fd" > "$LOCK_LOG" 2> "$PROTOCOL_LOG" &

assert_idle() {
  local before after
  before=$(events "$PROTOCOL_LOG" "$CONFIGURE_PATTERN")
  sleep 0.3 # real time: detect unsolicited configures while the locked output is static
  after=$(events "$PROTOCOL_LOG" "$CONFIGURE_PATTERN")
  if [[ "$before" != "$after" ]]; then
    echo "static lock surface received $((after-before)) redundant configures"
    return 1
  fi
}

await_lines "$LOCK_LOG" 'locked$' 1
"$ANDROMEDA" settle > /dev/null
assert_idle

cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'

[output.HEADLESS-1]
mode = "960x540"
scale = 1.0
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null
await_lines "$PROTOCOL_LOG" "${CONFIGURE_PATTERN}.*960, 540)" 1
"$ANDROMEDA" settle > /dev/null
assert_idle

echo unlock >&"$lock_fd"
await_lines "$LOCK_LOG" 'unlocked$' 1
echo "lock surface stayed idle, resized on output change, and unlocked"
