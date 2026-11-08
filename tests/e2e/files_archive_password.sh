#!/usr/bin/env bash
# Files v2 FB-ARCH (mockup files-v2-live.html §8-9): compress with a password, extract it back, drag out of an archive.
# Fails when:
#   - Compress (ZIP, password) does not write secret.zip with every entry encrypted and both items in it,
#   - Extract Here on it does not ask for the password, a wrong one is not refused, or the right one does not give
#     back the same files,
#   - the sheet does not estimate "About 15 MB in 2 parts" for 15 MB of video at Split 10 MB, Compress does not write
#     big.zip.001 (10 MB) and .002 only, or Extract Here on .001 does not give back the same files,
#   - dragging b.txt out of the open archive onto the other pane does not land the file with its content.
# SCALE=2 writes the 2x sheet shots for the mockup pair. Shots and logs go to $OUT (default ./artifacts/files-archive-password).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/files-archive-password}
source "$(dirname "$0")/lib.sh"
FILES=${FILES:-$ROOT/apps/files/build/andromeda-files}
FILEOPS=${FILEOPS:-$ROOT/system/target/debug/andromeda-fileops}
POINTER=$ROOT/compositor/build-debug/tests/pointer-client
APP=dev.andromeda.Files
HOME_DIR=$RUNTIME/home
SRC=$HOME_DIR/Work
DEST=$HOME_DIR/Out
boot_headless 1
if [[ ${SCALE:-1} == 2 ]]; then
  printf '\n[output.HEADLESS-1]\nmode = "2560x1440@60"\nscale = 2\n' >> "$RUNTIME/andromeda.toml"
  run "$ANDROMEDA" msg config-reload > /dev/null
  for _ in $(seq 100); do run "$ANDROMEDA" outputs --json | jq -e ".[0].scale == 2" > /dev/null && break; sleep 0.05; done
fi

fail() { echo "FAIL: $*"; exit 1; }
mkdir -p "$SRC/docs" "$DEST"
echo "alpha body" > "$SRC/docs/a.txt"
echo "beta body" > "$SRC/b.txt"
echo keep > "$DEST/keep.txt"

bus_info=$(dbus-daemon --config-file="$ROOT/tests/e2e/private-bus.conf" --fork --print-address=1 --print-pid=1)
BUS=$(sed -n 1p <<< "$bus_info")
BUS_PID=$(sed -n 2p <<< "$bus_info")
trap 'kill "$BUS_PID" 2> /dev/null || true; kill $(jobs -p) 2> /dev/null || true; wait 2> /dev/null; rm_runtime' EXIT
env_app=(env DESK_FILES_TEST=1 DBUS_SESSION_BUS_ADDRESS="$BUS" HOME="$HOME_DIR" XDG_DATA_HOME="$HOME_DIR/.local/share"
  XDG_STATE_HOME="$HOME_DIR/.local/state" XDG_CONFIG_HOME="$HOME_DIR/.config" XDG_CACHE_HOME="$HOME_DIR/.cache"
  LC_ALL=C.UTF-8)
run "${env_app[@]}" XDG_RUNTIME_DIR="$RUNTIME" "$FILEOPS" > "$OUT/fileops.log" 2>&1 &
run "${env_app[@]}" NOCTALIA_LOG_LEVEL=debug DESK_FILES_GEOMETRY=1 "$FILES" "$SRC" > "$OUT/files.log" 2>&1 &

win() { run "$ANDROMEDA" windows --json | jq -c "[.. | objects | select(.app_id? == \"$APP\")][0] // empty"; }
for _ in $(seq 100); do
  w=$(win)
  [[ -n $w && $(jq .w <<< "$w") -gt 0 ]] && break
  sleep 0.05
done
[[ -n ${w:-} ]] || fail "the Files window did not map"
WX=$(jq .x <<< "$w")
WY=$(jq .y <<< "$w")
run "$ANDROMEDA" settle > /dev/null

action() { # name [gvariant parameter list]
  DBUS_SESSION_BUS_ADDRESS=$BUS gdbus call --session -d "$APP" -o /dev/andromeda/Files \
    -m org.freedesktop.Application.ActivateAction "$1" "${2:-[]}" '{}' > /dev/null
}
wait_for() { # description, test command...
  local what=$1; shift
  for _ in $(seq 100); do "$@" && return 0; sleep 0.05; done
  fail "$what"
}
logged() { grep -qE -- "$1" "$OUT/files.log"; }
shot() { run "$ANDROMEDA" settle > /dev/null; sleep 0.4; run grim "$OUT/$1.png"; }
select_names() { # count, names gvariant
  for _ in $(seq 40); do
    action select "[<$2>]"
    grep -q "selected $1 of" "$OUT/files.log" && return 0
    sleep 0.05
  done
  fail "could not select $2"
}
zip_check() { # every entry encrypted (flag bit 0) and the names listed
  python3 -I - "$1" <<'PY'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as z:
    infos = [i for i in z.infolist() if not i.is_dir()]
    names = sorted(i.filename for i in infos)
    assert names == ["b.txt", "docs/a.txt"], names
    assert all(i.flag_bits & 1 for i in infos), "an entry is not encrypted"
PY
}
ptr() { run "$POINTER" 1280 720 tap 119 pause 100 "$@" > /dev/null; }
drag() { # from to: a drag in steps past the start threshold, a rest on the target, then the drop
  local from=($1) to=($2)
  ptr move "${from[0]}" "${from[1]}" pause 450 press 272 \
    move $((from[0] + 6)) $((from[1] + 6)) move $((from[0] + 20)) $((from[1] + 20)) \
    move $(((from[0] + to[0]) / 2)) $(((from[1] + to[1]) / 2)) move "${to[0]}" "${to[1]}" \
    pause 150 move $((to[0] + 1)) "${to[1]}" pause 50 release 272
}
# The geometry of a row in the left (smallest x) or right (largest x) pane.
pane_at() { # left|right, row name
  local order=-n
  [[ $1 == right ]] && order=-rn
  read -r x y <<< "$(tail -n +"${SPLIT_MARK:-1}" "$OUT/files.log" | grep -E "geometry item $2 " | awk '{print $(NF-1), $NF}' | sort $order | head -n 1)"
  [[ -n ${x:-} ]] || fail "no geometry for $2 in the $1 pane"
  echo "$((WX + x)) $((WY + y))"
}

wait_for "the daemon is not on the bus" \
  env DBUS_SESSION_BUS_ADDRESS="$BUS" gdbus introspect --session -d org.andromeda.FileOps1 -o /org/andromeda/FileOps1
wait_for "the folder did not list" logged "location $SRC"
select_names 2 "['docs','b.txt']"
action file.compress
action compress.set "[<['name','secret']>]"
action compress.set "[<['format','zip']>]"
action compress.set "[<['password','hunter2']>]"
shot compress-sheet
action compress.submit
wait_for "Compress did not finish" logged "job compress done"
zip_check "$SRC/secret.zip" || fail "secret.zip is not the encrypted archive of docs and b.txt"
echo "PASS compress with a password"

mkdir -p "$HOME_DIR/originals"
mv "$SRC/docs" "$SRC/b.txt" "$HOME_DIR/originals/"
select_names 1 "['secret.zip']"
action file.extract-here
wait_for "Extract Here did not ask for the password" logged "job extract failed"
wait_for "the password sheet did not show" bash -c 'grep -q "extract sheet password" "$1"' _ "$OUT/files.log"
action extract.password "[<'wrong'>]"
action extract.submit
wait_for "the wrong password was not refused" bash -c '[[ $(grep -c "job extract failed" "$1") -ge 2 ]]' _ "$OUT/files.log"
shot extract-wrong
action extract.password "[<'hunter2'>]"
action extract.submit
wait_for "the right password did not extract" logged "job extract done"
# Two top items extract into a folder named after the archive (Extract Here, mockup §9).
wait_for "secret/docs/a.txt is missing" test -f "$SRC/secret/docs/a.txt"
diff -r "$HOME_DIR/originals" "$SRC/secret" > /dev/null || fail "the extracted files differ: $(diff -r "$HOME_DIR/originals" "$SRC/secret")"
echo "PASS extract with the password"

# Split volumes (mockup §9 Split row): 15 MB of already-compressed bytes at 10 MB per part is 2 volumes.
mkdir -p "$SRC/big"
head -c 15000000 /dev/urandom > "$SRC/big/clip.mp4"
echo "small text" > "$SRC/big/notes.txt"
select_names 1 "['big']"
action file.compress
action compress.set "[<['split','10m']>]"
wait_for "the estimate did not show 2 parts" logged "compress estimate About 15 MB in 2 parts"
shot compress-split
action compress.submit
wait_long() { # description, test command...: 15 MB in the debug daemon takes seconds
  local what=$1; shift
  for _ in $(seq 600); do "$@" && return 0; sleep 0.05; done
  fail "$what"
}
done_twice() { [[ $(grep -c "job $1 done /" "$OUT/files.log") -ge 2 ]]; }
wait_long "the split Compress did not finish" done_twice compress
[[ -f $SRC/big.zip.001 && -f $SRC/big.zip.002 && ! -e $SRC/big.zip.003 && ! -e $SRC/big.zip ]] ||
  fail "the volumes are not big.zip.001 and .002: $(ls "$SRC")"
[[ $(stat -c %s "$SRC/big.zip.001") == 10000000 ]] || fail "part 1 is not 10 MB"
mv "$SRC/big" "$HOME_DIR/originals-big"
select_names 1 "['big.zip.001']"
action file.extract-here
wait_long "the split archive did not extract" done_twice extract
diff -r "$HOME_DIR/originals-big" "$SRC/big" > /dev/null || fail "the files from the volumes differ"
echo "PASS split volumes and extract them back"

select_names 1 "['secret.zip']"
action file.open
wait_for "Enter did not open the archive" logged "location archive:"
archive_row() { awk '/location archive:/ { f = 1 } f && /geometry item b.txt / { ok = 1 } END { exit !ok }' "$OUT/files.log"; }
wait_for "the archive lists no b.txt" archive_row
SPLIT_MARK=$(wc -l < "$OUT/files.log")
action view.split
wait_for "Split View did not open" logged "split view on"
action go "[<'$DEST'>]"
wait_for "the right pane did not go to Out" logged "location $DEST"
wait_for "the right pane lists no keep.txt" logged "geometry item keep.txt "
sleep 0.3 # the pane slide settles before the row positions are read
drag "$(pane_at left b.txt)" "$(pane_at right keep.txt)"
wait_for "the drag did not start from the archive" logged "drag out 1 items from secret.zip"
wait_for "b.txt did not land in Out" test -f "$DEST/b.txt"
[[ $(cat "$DEST/b.txt") == "beta body" ]] || fail "the dragged-out b.txt holds: $(cat "$DEST/b.txt")"
echo "PASS drag out of the archive"
