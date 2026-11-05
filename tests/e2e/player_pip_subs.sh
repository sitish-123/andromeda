#!/usr/bin/env bash
# Player P6: subtitles and picture in picture (docs/apps.md §5). Fails when:
#   - the external clip.srt next to the clip is not offered, or "player.subtitles" does not switch it off and on,
#   - "view.pip" does not map a dev.andromeda.Player.PiP window that the compositor pins (`pinned`),
#   - playback stops while the video moves into the PiP window or back,
#   - closing PiP does not return the video to the main window.
# Writes the app log and window lists to $OUT (default ./artifacts/player-pip-subs).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/player-pip-subs}
source "$(dirname "$0")/lib.sh"
PLAYER=${PLAYER:-$ROOT/apps/player/build/andromeda-player}
APP=dev.andromeda.Player
HOME_DIR=$RUNTIME/home
CLIP=$HOME_DIR/clip.mkv
boot_headless 1

fail() { echo "FAIL: $*"; exit 1; }
mkdir -p "$HOME_DIR"
ffmpeg -loglevel error -f lavfi -i testsrc=size=320x240:rate=25 -t 60 -c:v libx264 -preset ultrafast "$CLIP"
printf '1\n00:00:00,000 --> 00:01:00,000\nHello from the subtitle file\n' > "$HOME_DIR/clip.srt"

bus_info=$(dbus-daemon --session --fork --print-address=1 --print-pid=1)
BUS=$(sed -n 1p <<< "$bus_info")
BUS_PID=$(sed -n 2p <<< "$bus_info")
trap 'kill "$BUS_PID" 2> /dev/null || true; kill $(jobs -p) 2> /dev/null || true; wait 2> /dev/null; rm -rf "$RUNTIME"' EXIT

wait_for() { # description, test command...
  local what=$1; shift
  for _ in $(seq 100); do "$@" && return 0; sleep 0.05; done
  fail "$what"
}
win() { run "$ANDROMEDA" windows --json | jq -c "[.. | objects | select(.app_id? == \"$1\")][0] // empty"; }
mapped() { local w; w=$(win "$1"); [[ -n $w && $(jq .w <<< "$w") -gt 0 ]]; }
unmapped() { [[ -z $(win "$1") ]]; }
mpris() {
  DBUS_SESSION_BUS_ADDRESS=$BUS gdbus call --session -d org.mpris.MediaPlayer2.andromeda.Player \
    -o /org/mpris/MediaPlayer2 -m org.freedesktop.DBus.Properties.Get org.mpris.MediaPlayer2.Player "$1" 2> /dev/null
}
playing() { mpris PlaybackStatus | grep -q "'Playing'"; }
position_us() { mpris Position | grep -oE '[0-9]+' | tail -n 1; }
advances() { # playback moves on: the position grows within 1 s
  local before after
  before=$(position_us)
  for _ in $(seq 20); do
    sleep 0.05
    after=$(position_us)
    [[ -n $before && -n $after ]] && (( after > before )) && return 0
  done
  return 1
}
action() {
  DBUS_SESSION_BUS_ADDRESS=$BUS gdbus call --session -d "$APP" -o /dev/andromeda/Player \
    -m org.freedesktop.Application.ActivateAction "$1" "${2:-[]}" '{}' > /dev/null
}
log_count() { sed 's/\x1b\[[0-9;]*m//g' "$OUT/player.log" | grep -c "$1" || true; }
count_above() { (( $(log_count "$1") > $2 )); }

run env DBUS_SESSION_BUS_ADDRESS="$BUS" HOME="$HOME_DIR" XDG_STATE_HOME="$HOME_DIR/.local/state" \
  NOCTALIA_LOG_LEVEL=debug "$PLAYER" "$CLIP" > "$OUT/player.log" 2>&1 &
wait_for "the player window did not map" mapped "$APP"
wait_for "MPRIS did not report Playing" playing

# Subtitles: whatever the start state, two switches must show both an off and an on state.
offs=$(log_count "subtitles off")
ons=$(log_count "subtitles [^o]")
action player.subtitles
action player.subtitles
wait_for "the subtitle switch never reached off" count_above "subtitles off" "$offs"
wait_for "the subtitle switch never showed clip.srt" count_above "subtitles [^o]" "$ons"
echo "PASS subtitles off and on"

action view.pip
wait_for "the PiP window did not map" mapped "$APP.PiP"
run "$ANDROMEDA" windows --json > "$OUT/windows-pip.json"
[[ $(win "$APP.PiP" | jq -r '.pinned') == true ]] || fail "the PiP window is not pinned"
wait_for "the video did not move to the PiP window" count_above "video moved to a new window" 0
wait_for "playback stopped in PiP" advances
echo "PASS picture in picture (pinned, playing)"

action view.pip
wait_for "the PiP window did not close" unmapped "$APP.PiP"
mapped "$APP" || fail "the main window is gone after PiP"
wait_for "the video did not come back" count_above "video moved to a new window" 1
wait_for "playback stopped after PiP" advances
echo "PASS back from picture in picture"
