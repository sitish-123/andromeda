#!/usr/bin/env bash
# A bind's release_action runs when its key comes up, not before: the power-key hold depends on it.
set -euo pipefail

readonly KEY_P=25
readonly POINTER="${ANDROMEDA_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly MARKS="$ANDROMEDA_RUNTIME_DIR/release-marks"
: > "$MARKS"

cat >> "$ANDROMEDA_CONFIG" <<EOF

[keybinds]
"Mod+P" = { action = "spawn:sh -c 'echo down >> $MARKS'", release_action = "spawn:sh -c 'echo up >> $MARKS'", repeat = false }
EOF
"$ANDROMEDA" msg config-reload > /dev/null

wait_marks() {
  for _ in $(seq 50); do
    [[ $(tr '\n' ' ' < "$MARKS") == "$1" ]] && return 0
    sleep 0.02
  done
  echo "expected marks '$1', got '$(tr '\n' ' ' < "$MARKS")'"
  return 1
}

# One client keeps the virtual keyboard alive from press to release.
"$POINTER" 1280 720 mod logo pause 100 key-press "$KEY_P" pause 1500 key-release "$KEY_P" mod none pause 100 \
  > "$ANDROMEDA_RUNTIME_DIR/release-pointer.log" 2>&1 &
pointer=$!
wait_marks "down "
wait "$pointer"
wait_marks "down up "
echo "release_action ran once, after the key came up"
