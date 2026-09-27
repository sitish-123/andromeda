#!/usr/bin/env bash
# A window on the active workspace is suspended and gets no frame callbacks while the session is locked and while its
# output is powered off, and resumes with its frames after each; a game keeps its slow tick behind the lock.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly CLIENT="${UMBRIEL_BACKGROUND_CLIENT:-./build-debug/tests/background-client}"
readonly LOCK_CLIENT="${UMBRIEL_LOCK_CLIENT:-./build-debug/tests/lock-client}"
readonly NORMAL_LOG="$UMBRIEL_RUNTIME_DIR/visible-normal.log"
readonly GAME_LOG="$UMBRIEL_RUNTIME_DIR/visible-game.log"
readonly LOCK_LOG="$UMBRIEL_RUNTIME_DIR/lock-client.log"
readonly LOCK_FIFO="$UMBRIEL_RUNTIME_DIR/lock-control"

"$CLIENT" visible-normal > "$NORMAL_LOG" 2>&1 &
CONTENT_TYPE=game "$CLIENT" visible-game > "$GAME_LOG" 2>&1 &
await_events "$NORMAL_LOG" mapped 1
await_events "$GAME_LOG" mapped 1
"$UMBRIEL" settle

frames() { events "$1" frame; }

# Frames a log gains over one second.
frames_in_a_second() {
  local before
  before=$(frames "$1")
  sleep 1 # real time: the background tick runs on a wall-clock timer
  echo $(($(frames "$1") - before))
}

expect_resumed_with_frames() {
  await_events "$NORMAL_LOG" resumed "$1" "the window after $2"
  local back
  back=$(frames "$NORMAL_LOG")
  for _ in $(seq 40); do
    (($(frames "$NORMAL_LOG") > back + 2)) && return 0
    sleep 0.05
  done
  echo "frames did not resume after $2"
  return 1
}

mkfifo "$LOCK_FIFO"
exec {lock_fd}<> "$LOCK_FIFO"
"$LOCK_CLIENT" <&"$lock_fd" > "$LOCK_LOG" 2>&1 &
await_events "$LOCK_LOG" locked 1 "the lock client"
await_events "$NORMAL_LOG" suspended 1 "the window behind the lock"
if grep -q '^suspended$' "$GAME_LOG"; then
  echo "the game was suspended behind the lock"
  exit 1
fi
locked_frames=$(frames_in_a_second "$NORMAL_LOG")
game_frames=$(frames_in_a_second "$GAME_LOG")
if ((locked_frames > 1)); then
  echo "the window kept receiving frames behind the lock: $locked_frames in 1 s"
  exit 1
fi
if ((game_frames < 5)); then
  echo "the game lost its tick behind the lock: $game_frames frames in 1 s"
  exit 1
fi

echo unlock >&"$lock_fd"
await_events "$LOCK_LOG" unlocked 1 "the lock client"
expect_resumed_with_frames 1 "unlocking"

"$UMBRIEL" msg dpms-off > /dev/null
await_events "$NORMAL_LOG" suspended 2 "the window on a powered-off output"
off_frames=$(frames_in_a_second "$NORMAL_LOG")
if ((off_frames > 1)); then
  echo "the window kept receiving frames with its output off: $off_frames in 1 s"
  exit 1
fi

"$UMBRIEL" msg dpms-on > /dev/null
expect_resumed_with_frames 2 "powering the output on"

echo "suspended behind the lock ($locked_frames frames/s, game kept $game_frames) and with the output off ($off_frames frames/s); resumed after each"
