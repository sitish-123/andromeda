#!/usr/bin/env bash
# Player P5: plays a generated clip, reports it over MPRIS, inhibits idle while video plays, and resumes after a
# restart (docs/apps.md §5). Fails when:
#   - the window does not map or MPRIS PlaybackStatus does not become Playing,
#   - a seek to 30 s does not show in MPRIS Position, or idle is not inhibited while playing / released on pause,
#   - after MPRIS Quit and a relaunch of the same file, playback does not start within 1 s of 30 s.
# Writes the app logs and state.json to $OUT (default ./artifacts/player-resume).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/player-resume}
source "$(dirname "$0")/lib.sh"
PLAYER=${PLAYER:-$ROOT/apps/player/build/andromeda-player}
APP=dev.andromeda.Player
HOME_DIR=$RUNTIME/home
CLIP=$HOME_DIR/clip.mkv
STATE=$HOME_DIR/.local/state/andromeda/$APP/state.json
boot_headless 1

fail() { echo "FAIL: $*"; exit 1; }
mkdir -p "$HOME_DIR"
ffmpeg -loglevel error -f lavfi -i testsrc=size=320x240:rate=25 -f lavfi -i sine=frequency=440 -t 60 \
  -c:v libx264 -preset ultrafast -c:a aac "$CLIP"

bus_info=$(dbus-daemon --session --fork --print-address=1 --print-pid=1)
BUS=$(sed -n 1p <<< "$bus_info")
BUS_PID=$(sed -n 2p <<< "$bus_info")
trap 'kill "$BUS_PID" 2> /dev/null || true; kill $(jobs -p) 2> /dev/null || true; wait 2> /dev/null; rm -rf "$RUNTIME"' EXIT
env_app=(env DBUS_SESSION_BUS_ADDRESS="$BUS" HOME="$HOME_DIR" XDG_STATE_HOME="$HOME_DIR/.local/state"
  XDG_CONFIG_HOME="$HOME_DIR/.config" NOCTALIA_LOG_LEVEL=debug)

wait_for() { # description, test command...
  local what=$1; shift
  for _ in $(seq 100); do "$@" && return 0; sleep 0.05; done
  fail "$what"
}
win() { run "$ANDROMEDA" windows --json | jq -c "[.. | objects | select(.app_id? == \"$APP\")][0] // empty"; }
mapped() { local w; w=$(win); [[ -n $w && $(jq .w <<< "$w") -gt 0 ]]; }
unmapped() { [[ -z $(win) ]]; }
mpris() { # property
  DBUS_SESSION_BUS_ADDRESS=$BUS gdbus call --session -d org.mpris.MediaPlayer2.andromeda.Player \
    -o /org/mpris/MediaPlayer2 -m org.freedesktop.DBus.Properties.Get org.mpris.MediaPlayer2.Player "$1" 2> /dev/null
}
playing() { mpris PlaybackStatus | grep -q "'Playing'"; }
paused() { mpris PlaybackStatus | grep -q "'Paused'"; }
position_s() { mpris Position | grep -oE '[0-9]+' | tail -n 1 | awk '{print int($1 / 1000000)}'; }
position_between() { local p; p=$(position_s); [[ -n $p ]] && (( p >= $1 && p <= $2 )); }
logged() { grep -q "$2" "$OUT/$1.log"; }
start_player() { # name
  run "${env_app[@]}" "$PLAYER" "$CLIP" > "$OUT/$1.log" 2>&1 &
  wait_for "the player window did not map ($1)" mapped
  wait_for "MPRIS did not report Playing ($1)" playing
}
action() {
  DBUS_SESSION_BUS_ADDRESS=$BUS gdbus call --session -d "$APP" -o /dev/andromeda/Player \
    -m org.freedesktop.Application.ActivateAction "$1" "${2:-[]}" '{}' > /dev/null
}
mpris_call() {
  DBUS_SESSION_BUS_ADDRESS=$BUS gdbus call --session -d org.mpris.MediaPlayer2.andromeda.Player \
    -o /org/mpris/MediaPlayer2 -m "$1" > /dev/null
}

start_player first
echo "PASS MPRIS Playing"
wait_for "idle was not inhibited while video plays" logged first "idle inhibited"
action seek "[<'30'>]"
wait_for "MPRIS Position did not reach 30 s after the seek" position_between 30 40
mpris_call org.mpris.MediaPlayer2.Player.Pause
wait_for "MPRIS did not report Paused" paused
wait_for "idle was not released on pause" logged first "idle released"
echo "PASS seek, idle inhibit and release"
paused_at=$(position_s)
mpris_call org.mpris.MediaPlayer2.Quit
wait_for "the player did not quit" unmapped
cp "$STATE" "$OUT/state.json" 2> /dev/null || fail "no state.json after quit"

start_player second
position_between "$((paused_at - 1))" "$((paused_at + 1))" \
  || fail "relaunch started at $(position_s) s, not near $paused_at s"
echo "PASS resume at ${paused_at} s"
