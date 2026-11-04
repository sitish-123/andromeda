#!/usr/bin/env bash
# dsk_shell_v1 key enters the key pipeline after the compositor bindings: the focused window receives the key even when
# the bind's modifier is held and a bind exists for it, and the bind does not run. A physical tap of the same key is the
# control that runs the bind.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly POINTER="${ANDROMEDA_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly OBSERVER="${ANDROMEDA_SEAT_LOG_CLIENT:-./build-debug/tests/seat-log-client}"
readonly SHELL_CLIENT="${ANDROMEDA_DESKTOP_CLIENT:-./build-debug/tests/desktop-client}"
readonly SHELL_LOG="$ANDROMEDA_RUNTIME_DIR/shell-events.log"
readonly WINDOW_LOG="$ANDROMEDA_RUNTIME_DIR/window.log"
readonly KEYBOARD_LOG="$ANDROMEDA_RUNTIME_DIR/keyboard.log"
readonly SHELL_FIFO="$ANDROMEDA_RUNTIME_DIR/shell-control"
readonly KEYBOARD_FIFO="$ANDROMEDA_RUNTIME_DIR/keyboard-control"
readonly KEY_X=45
readonly KEY_Y=21
readonly KEYSYM_X=120
readonly KEYSYM_Y=121

cat >> "$ANDROMEDA_CONFIG" <<'CONFIG'

[keybinds]
"Mod+X" = "shell:bound-x"
CONFIG
"$ANDROMEDA" msg config-reload > /dev/null

mkfifo "$SHELL_FIFO" "$KEYBOARD_FIFO"
exec {shell_fd}<> "$SHELL_FIFO"
exec {keyboard_fd}<> "$KEYBOARD_FIFO"
"$POINTER" 1280 720 keyboard-only mod logo mark ready hold tap "$KEY_X" mark tapped <&"$keyboard_fd" > "$KEYBOARD_LOG" 2>&1 &
await_events "$KEYBOARD_LOG" ready 1 "the keyboard holder"
"$OBSERVER" key-window > "$WINDOW_LOG" 2>&1 &
await_events "$WINDOW_LOG" keyboard-enter 1 "the window's keyboard focus"
"$SHELL_CLIENT" shell-events <&"$shell_fd" > "$SHELL_LOG" 2>&1 &
await_events "$SHELL_LOG" ready 1 "the shell stand-in"

printf 'key %s 1\nkey %s 0\nkey %s 1\nkey %s 0\nmark sent\n' "$KEYSYM_X" "$KEYSYM_X" "$KEYSYM_Y" "$KEYSYM_Y" >&"$shell_fd"
await_events "$SHELL_LOG" mark\ sent 1 "the shell's request round trip"
await_events "$WINDOW_LOG" "keyboard-key code=$KEY_Y state=released" 1 "the barrier key"
await_events "$WINDOW_LOG" "keyboard-key code=$KEY_X state=pressed" 1 "the synthetic key"
if [[ $(events "$SHELL_LOG" action) != 0 ]]; then
  echo "the synthetic key ran a binding: $(tr '\n' '|' < "$SHELL_LOG")"
  exit 1
fi

echo go >&"$keyboard_fd"
await_events "$KEYBOARD_LOG" tapped 1 "the physical tap"
await_events "$SHELL_LOG" "action bound-x" 1 "the control: the physical tap runs the bind"
if [[ $(events "$WINDOW_LOG" "keyboard-key code=$KEY_X") != 2 ]]; then
  echo "the bound physical tap reached the window: $(tr '\n' '|' < "$WINDOW_LOG")"
  exit 1
fi
echo "dsk_shell key reached the focused window after the bindings and ran no bind"
