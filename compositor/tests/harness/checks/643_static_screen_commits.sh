#!/usr/bin/env bash
# A static screen commits nothing, so panel self-refresh can stay on: with a panel, two tiled windows after a focus
# change, and a game redrawing on a hidden workspace, the output makes no commit over three seconds once settled.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly WINDOW_CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly LAYER_CLIENT="${UMBRIEL_LAYER_CLIENT:-./build-debug/tests/layer-client}"
readonly BACKGROUND_CLIENT="${UMBRIEL_BACKGROUND_CLIENT:-./build-debug/tests/background-client}"
readonly WINDOW_SECONDS=3

"$LAYER_CLIENT" HEADLESS-1 40 > "$UMBRIEL_RUNTIME_DIR/panel.log" 2>&1 &
await_events "$UMBRIEL_RUNTIME_DIR/panel.log" ready 1 "the panel"

CONTENT_TYPE=game "$BACKGROUND_CLIENT" hidden-game > "$UMBRIEL_RUNTIME_DIR/game.log" 2>&1 &
await_events "$UMBRIEL_RUNTIME_DIR/game.log" mapped 1 "the game"
"$UMBRIEL" settle
"$UMBRIEL" msg workspace-switch:2 > /dev/null

for title in static-a static-b; do
  "$WINDOW_CLIENT" "$title" > "$UMBRIEL_RUNTIME_DIR/$title.log" 2>&1 &
  await_events "$UMBRIEL_RUNTIME_DIR/$title.log" mapped 1 "$title"
done
"$UMBRIEL" msg window-focus-left > /dev/null
"$UMBRIEL" settle
game_workspace=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "hidden-game") | .workspace')
if [[ $game_workspace == $("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "static-a") | .workspace') ]]; then
  echo "the game shares the visible workspace $game_workspace"
  exit 1
fi

commits() { "$UMBRIEL" output-commits --json | jq -r '."HEADLESS-1" // .ok."HEADLESS-1"'; }
game_frames() { events "$UMBRIEL_RUNTIME_DIR/game.log" frame; }
before=$(commits)
game_before=$(game_frames)
sleep "$WINDOW_SECONDS" # real time: nothing may commit within the window
idle=$(($(commits) - before))
game=$(($(game_frames) - game_before))
if ((idle != 0)); then
  echo "a static screen committed $idle times in $WINDOW_SECONDS s"
  exit 1
fi
if ((game < WINDOW_SECONDS * 5)); then
  echo "the hidden game stopped redrawing ($game frames), so the window proves nothing about it"
  exit 1
fi

echo "static screen: 0 commits in $WINDOW_SECONDS s while a hidden game drew $game frames"
