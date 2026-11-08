#!/usr/bin/env bash
# Captures the player library pages for comparison with .agent/DESIGN.md "Player music library (P6b UI) direction A":
# Albums grid (one album with a cover.png, one with no art), album page, Now Playing.
# Writes $OUT/{albums,album,now-playing}.png (default ./artifacts/player-library-shots).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/player-library-shots}
source "$(dirname "$0")/lib.sh"
PLAYER=${PLAYER:-$ROOT/apps/player/build/andromeda-player}
APP=dev.andromeda.Player
HOME_DIR=$RUNTIME/home
MUSIC=$HOME_DIR/Music
boot_headless 1

fail() { echo "FAIL: $*"; exit 1; }
mkdir -p "$MUSIC/Covered" "$MUSIC/Bare" "$HOME_DIR/.config"
printf 'XDG_MUSIC_DIR="%s"\n' "$MUSIC" > "$HOME_DIR/.config/user-dirs.dirs"
ffmpeg -y -loglevel error -f lavfi -i "testsrc=size=400x400" -frames:v 1 "$MUSIC/Covered/cover.png"
for n in 1 2 3; do
  ffmpeg -y -loglevel error -f lavfi -i "sine=frequency=440:duration=$((n * 20))" -c:a flac -metadata title="Track $n" \
    -metadata artist=Andromeda -metadata album="Covered Album" -metadata date=2026 -metadata track=$n "$MUSIC/Covered/0$n.flac"
done
ffmpeg -y -loglevel error -f lavfi -i "sine=frequency=220:duration=5" -c:a flac -metadata title="A long song title that has to ellipsize" \
  -metadata artist="Someone With A Long Name" -metadata album="An Album Title Long Enough To Ellipsize" "$MUSIC/Bare/01.flac"

bus_info=$(dbus-daemon --session --fork --print-address=1 --print-pid=1)
BUS=$(sed -n 1p <<< "$bus_info")
BUS_PID=$(sed -n 2p <<< "$bus_info")
trap 'kill "$BUS_PID" 2> /dev/null || true; kill $(jobs -p) 2> /dev/null || true; wait 2> /dev/null; rm_runtime' EXIT

wait_for() { # description, test command...
  local what=$1; shift
  for _ in $(seq 200); do "$@" && return 0; sleep 0.05; done
  fail "$what"
}
logged() { sed 's/\x1b\[[0-9;]*m//g' "$OUT/player.log" | grep -q "$1"; }
action() {
  DBUS_SESSION_BUS_ADDRESS=$BUS gdbus call --session -d "$APP" -o /dev/andromeda/Player \
    -m org.freedesktop.Application.ActivateAction "$1" "${2:-[]}" '{}' > /dev/null
}

run env DBUS_SESSION_BUS_ADDRESS="$BUS" HOME="$HOME_DIR" XDG_CONFIG_HOME="$HOME_DIR/.config" \
  XDG_STATE_HOME="$HOME_DIR/.local/state" XDG_CACHE_HOME="$HOME_DIR/.cache" NOCTALIA_LOG_LEVEL=debug \
  ANDROMEDA_PLAYER_MPV_OPTIONS="ao=null" "$PLAYER" > "$OUT/player.log" 2>&1 &
wait_for "the library did not list 2 albums" logged "library: 2 albums, 4 tracks"
sleep 1 # images decode after the first frame
run grim "$OUT/albums.png"
action library.show-album "[<'Covered Album'>]"
sleep 0.5
run grim "$OUT/album.png"
action library.play-album "[<'Covered Album'>]"
wait_for "the album did not start" logged "now playing Track 1"
sleep 0.5
run grim "$OUT/now-playing.png"
ls "$OUT"/*.png
