#!/usr/bin/env bash
# Player P6b: the music library (docs/apps.md §5). Three tagged FLACs cut sample-exact from one continuous sine form
# one album; a playlist in ~/Music/Playlists lists two of them in reverse. mpv writes its audio to a wav (ao=pcm).
# Fails when:
#   - the library does not show 1 album with 3 tracks and 1 playlist,
#   - album playback leaves a gap of 5 ms or more at a track join (gapless) or changes the length,
#   - the album ReplayGain (-6.02 dB) does not halve the level,
#   - the playlist does not play its entries in file order,
#   - MPRIS Next does not move to the next track, or Previous (in the first 3 s) does not move back.
# Writes the app logs, the wavs and the analysis to $OUT (default ./artifacts/player-library).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/player-library}
source "$(dirname "$0")/lib.sh"
PLAYER=${PLAYER:-$ROOT/apps/player/build/andromeda-player}
APP=dev.andromeda.Player
HOME_DIR=$RUNTIME/home
MUSIC=$HOME_DIR/Music
RATE=44100
boot_headless 1

fail() { echo "FAIL: $*"; exit 1; }
mkdir -p "$MUSIC/Album" "$MUSIC/Playlists" "$HOME_DIR/.config"
printf 'XDG_MUSIC_DIR="%s"\n' "$MUSIC" > "$HOME_DIR/.config/user-dirs.dirs"
ffmpeg -y -loglevel error -f lavfi -i "sine=frequency=440:sample_rate=$RATE:duration=9" -ac 1 -c:a pcm_s16le "$OUT/source.wav"
for n in 1 2 3; do
  start=$(( (n - 1) * 3 * RATE ))
  ffmpeg -y -loglevel error -i "$OUT/source.wav" -af "atrim=start_sample=$start:end_sample=$(( start + 3 * RATE ))" \
    -c:a flac -metadata title="Track $n" -metadata artist=Andromeda -metadata album="Test Album" -metadata track=$n \
    -metadata REPLAYGAIN_ALBUM_GAIN="-6.02 dB" -metadata REPLAYGAIN_TRACK_GAIN="-6.02 dB" "$MUSIC/Album/0$n.flac"
done
printf '#EXTM3U\n../Album/03.flac\n../Album/01.flac\n' > "$MUSIC/Playlists/mix.m3u8"

bus_info=$(dbus-daemon --config-file="$ROOT/tests/e2e/private-bus.conf" --fork --print-address=1 --print-pid=1)
BUS=$(sed -n 1p <<< "$bus_info")
BUS_PID=$(sed -n 2p <<< "$bus_info")
trap 'kill "$BUS_PID" 2> /dev/null || true; kill $(jobs -p) 2> /dev/null || true; wait 2> /dev/null; rm_runtime' EXIT

wait_for() { # description, test command...
  local what=$1; shift
  for _ in $(seq 200); do "$@" && return 0; sleep 0.05; done
  fail "$what"
}
win() { run "$ANDROMEDA" windows --json | jq -c "[.. | objects | select(.app_id? == \"$APP\")][0] // empty"; }
mapped() { local w; w=$(win); [[ -n $w && $(jq .w <<< "$w") -gt 0 ]]; }
unmapped() { [[ -z $(win) ]]; }
clean_log() { sed 's/\x1b\[[0-9;]*m//g' "$OUT/$1.log"; }
logged() { clean_log "$1" | grep -q "$2"; }
mpris() {
  DBUS_SESSION_BUS_ADDRESS=$BUS gdbus call --session -d org.mpris.MediaPlayer2.andromeda.Player \
    -o /org/mpris/MediaPlayer2 -m "$@" > /dev/null 2>&1
}
ended() { # keep-open: the last track ends paused at eof
  DBUS_SESSION_BUS_ADDRESS=$BUS gdbus call --session -d org.mpris.MediaPlayer2.andromeda.Player \
    -o /org/mpris/MediaPlayer2 -m org.freedesktop.DBus.Properties.Get org.mpris.MediaPlayer2.Player PlaybackStatus \
    2> /dev/null | grep -q "'Paused'"
}
action() {
  DBUS_SESSION_BUS_ADDRESS=$BUS gdbus call --session -d "$APP" -o /dev/andromeda/Player \
    -m org.freedesktop.Application.ActivateAction "$1" "${2:-[]}" '{}' > /dev/null
}
start_player() { # name [mpv options]; ao=pcm writes faster than real time, so a timed run passes ao=null
  run env DBUS_SESSION_BUS_ADDRESS="$BUS" HOME="$HOME_DIR" XDG_CONFIG_HOME="$HOME_DIR/.config" \
    XDG_STATE_HOME="$HOME_DIR/.local/state" XDG_CACHE_HOME="$HOME_DIR/.cache" NOCTALIA_LOG_LEVEL=debug \
    ANDROMEDA_PLAYER_MPV_OPTIONS="${2:-ao=pcm,ao-pcm-file=$OUT/$1.wav,audio-format=s16,audio-samplerate=$RATE,audio-channels=mono}" \
    "$PLAYER" > "$OUT/$1.log" 2>&1 &
  wait_for "the player window did not map ($1)" mapped
  wait_for "the library did not list 1 album, 3 tracks, 1 playlist ($1)" \
    logged "$1" "library: 1 albums, 3 tracks, 1 playlists"
}
stop_player() {
  mpris org.mpris.MediaPlayer2.Quit
  wait_for "the player did not quit" unmapped
}

start_player album
echo "PASS library: 1 album, 3 tracks, 1 playlist"
action library.play-album "[<'Test Album'>]"
wait_for "the album did not reach Track 3" logged album "now playing Track 3"
wait_for "the album did not end" ended
stop_player
python3 - "$OUT/source.wav" "$OUT/album.wav" "$RATE" > "$OUT/album-analysis.txt" << 'EOF' || { cat "$OUT/album-analysis.txt"; fail "album audio check"; }
import array, math, sys, wave
def samples(path):
    with wave.open(path) as w:
        assert w.getsampwidth() == 2 and w.getnchannels() == 1, (w.getsampwidth(), w.getnchannels())
        return array.array("h", w.readframes(w.getnframes()))
src, out, rate = samples(sys.argv[1]), samples(sys.argv[2]), int(sys.argv[3])
# Trim the silence mpv may write before the first sample.
start = next(i for i, x in enumerate(out) if abs(x) > 64)
out = out[start:]
end = len(out) - next(i for i, x in enumerate(reversed(out)) if abs(x) > 64)
out = out[:end]
longest = run = 0
for x in out:
    run = run + 1 if abs(x) <= 64 else 0
    longest = max(longest, run)
rms = lambda a: math.sqrt(sum(x * x for x in a) / len(a))
ratio = rms(out) / rms(src)
print(f"length {len(out) / rate:.4f} s (source {len(src) / rate:.4f} s), longest quiet run {longest / rate * 1000:.2f} ms, level ratio {ratio:.3f}")
assert longest < 0.005 * rate, "a gap of 5 ms or more"
assert abs(len(out) - len(src)) < 0.005 * rate, "the length changed: a gap or a cut"
assert 0.45 < ratio < 0.55, "album gain -6.02 dB did not halve the level"
EOF
cat "$OUT/album-analysis.txt"
echo "PASS gapless album with ReplayGain"

start_player playlist
action library.play-playlist "[<'mix'>]"
wait_for "the playlist did not reach its second entry" logged playlist "now playing Track 1"
order=$(clean_log playlist | grep -o "now playing Track [0-9]" | tr '\n' ' ')
[[ $order == "now playing Track 3 now playing Track 1 " ]] || fail "playlist order: $order"
stop_player
echo "PASS playlist order"

start_player nav ao=null
action library.play-album "[<'Test Album'>]"
wait_for "the album did not start" logged nav "now playing Track 1"
mpris org.mpris.MediaPlayer2.Player.Next
wait_for "MPRIS Next did not reach Track 2" logged nav "now playing Track 2"
mpris org.mpris.MediaPlayer2.Player.Previous
back() { order=$(clean_log nav | grep -o "now playing Track [0-9]" | tr '\n' ' '); [[ $order == "now playing Track 1 now playing Track 2 now playing Track 1 "* ]]; }
wait_for "MPRIS Previous did not return to Track 1" back
stop_player
echo "PASS MPRIS Next and Previous"
