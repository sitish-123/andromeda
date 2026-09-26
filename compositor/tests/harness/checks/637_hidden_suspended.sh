#!/usr/bin/env bash
# A window on a hidden workspace is suspended and stops receiving frame callbacks, while a game (by content type or
# by a background_frames rule) keeps a slow tick so its network loop survives alt-tab. Returning to the workspace
# resumes the window and its frames.
set -euo pipefail

readonly CLIENT="${UMBRIEL_BACKGROUND_CLIENT:-./build-debug/tests/background-client}"
NORMAL_LOG="$UMBRIEL_RUNTIME_DIR/background-normal.log"
GAME_LOG="$UMBRIEL_RUNTIME_DIR/background-game.log"
RULED_LOG="$UMBRIEL_RUNTIME_DIR/background-ruled.log"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[[window_rule]]
match.title = "^background-ruled$"
background_frames = true
EOF
"$UMBRIEL" msg config-reload > /dev/null

"$CLIENT" background-normal > "$NORMAL_LOG" 2>&1 &
CONTENT_TYPE=game "$CLIENT" background-game > "$GAME_LOG" 2>&1 &
"$CLIENT" background-ruled > "$RULED_LOG" 2>&1 &

for log in "$NORMAL_LOG" "$GAME_LOG" "$RULED_LOG"; do
  for _ in $(seq 40); do
    grep -q '^mapped$' "$log" && break
    sleep 0.05
  done
  grep -q '^mapped$' "$log" || { echo "client never mapped: $(cat "$log")"; exit 1; }
done
"$UMBRIEL" settle

frames() { grep -c '^frame$' "$1" || true; }

"$UMBRIEL" msg workspace-switch:2 > /dev/null
"$UMBRIEL" settle
for _ in $(seq 40); do
  grep -q '^suspended$' "$NORMAL_LOG" && break
  sleep 0.05
done
grep -q '^suspended$' "$NORMAL_LOG" || { echo "hidden window was not suspended"; exit 1; }
for log in "$GAME_LOG" "$RULED_LOG"; do
  if grep -q '^suspended$' "$log"; then
    echo "hidden game was suspended: $log"
    exit 1
  fi
done

normal_before=$(frames "$NORMAL_LOG")
game_before=$(frames "$GAME_LOG")
ruled_before=$(frames "$RULED_LOG")
sleep 1 # real time: the background tick runs on a wall-clock timer
normal_hidden=$(($(frames "$NORMAL_LOG") - normal_before))
game_hidden=$(($(frames "$GAME_LOG") - game_before))
ruled_hidden=$(($(frames "$RULED_LOG") - ruled_before))
if ((normal_hidden > 1)); then
  echo "hidden window kept receiving frames: $normal_hidden in 1 s"
  exit 1
fi
if ((game_hidden < 5 || ruled_hidden < 5)); then
  echo "hidden game lost its background tick: content type $game_hidden, rule $ruled_hidden frames in 1 s"
  exit 1
fi

"$UMBRIEL" msg workspace-switch:1 > /dev/null
"$UMBRIEL" settle
for _ in $(seq 40); do
  grep -q '^resumed$' "$NORMAL_LOG" && break
  sleep 0.05
done
grep -q '^resumed$' "$NORMAL_LOG" || { echo "window stayed suspended after returning"; exit 1; }
normal_back=$(frames "$NORMAL_LOG")
for _ in $(seq 40); do
  (($(frames "$NORMAL_LOG") > normal_back + 2)) && break
  sleep 0.05
done
(($(frames "$NORMAL_LOG") > normal_back + 2)) || { echo "frames did not resume after returning"; exit 1; }

echo "hidden window suspended with $normal_hidden frames; games kept $game_hidden (content type) and $ruled_hidden (rule) frames/s; resumed"
