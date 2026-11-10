#!/usr/bin/env bash
# Activity v3 design shots next to .agent/mockups/activity-v3.html §A: Memory pane with the problem card in light,
# CPU pane with Zen Browser selected in dark, each at 1100 and 1600 px wide. Two foot windows run in real scopes
# (rows with a window); the other rows are mock units whose cgroup files live in a fake tree (ANDROMEDA_CGROUP_ROOT),
# with a loop that adds CPU time so CPU shows. Shots only; the mockup is the reference. Images in $OUT
# (default ./artifacts/activity-v3).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/activity-v3}
source "$(dirname "$0")/lib.sh"
REAL_RUNTIME=/run/user/$(id -u)
SCOPES=("app-foot-$$-1.scope" "app-foot-$$-2.scope")
trap 'for s in "${SCOPES[@]}"; do XDG_RUNTIME_DIR=$REAL_RUNTIME systemctl --user stop "$s" > /dev/null 2>&1 || true; done; kill $(jobs -p) 2>/dev/null || true; wait 2>/dev/null; rm_runtime' EXIT
boot_headless 1
printf '\n[output.HEADLESS-1]\nmode = "1720x1000@60"\n\n[[window_rule]]\nblur = true\n' >> "$RUNTIME/andromeda.toml"
# Desktop entries for the mock apps this machine may not have installed: rows show their names and icons.
APPS=$RUNTIME/home/.local/share/applications
mkdir -p "$APPS"
for spec in "org.telegram.desktop:Telegram:telegram" "discord:Discord:discord" "krita:Krita:krita" "syncthing:Syncthing:syncthing"; do
  IFS=: read -r id name icon <<< "$spec"
  printf '[Desktop Entry]\nType=Application\nName=%s\nIcon=%s\nExec=true\n' "$name" "$icon" > "$APPS/$id.desktop"
done
CG=$RUNTIME/cgroup
BASE=$CG/user.slice/user-1000.slice/user@1000.service
fake() { # unit slice memory_mb cpu_usec pressure read_bps
  local d=$BASE/$2/$1
  mkdir -p "$d"
  printf 'usage_usec %s\nuser_usec 1\nsystem_usec 1\n' "$4" > "$d/cpu.stat"
  echo $(($3 * 1024 * 1024)) > "$d/memory.current"
  printf 'some avg10=%s avg60=0.00 avg300=0.00 total=1\n' "$5" > "$d/memory.pressure"
  printf '8:0 rbytes=%s wbytes=%s rios=1 wios=1 dbytes=0 dios=0\n' "${6:-0}" "${6:-0}" > "$d/io.stat"
}
fake 'app-zen\x2dbrowser@0f1e2d3c.service' app.slice 9600 9000000000 14.00 1200000
fake opencode.service app.slice 480 120000000 0.00
fake pipewire.service session.slice 42 30000000 0.00
fake app-andromeda-1835-17.scope app.slice 210 1300000000 0.00
fake socketed.service app.slice 12 1000000 0.00
UNITS=("app-org.telegram.desktop@1.service:520:230000000" "app-spotify@2.service:410:720000000"
  "app-discord@3.service:380:320000000" 'app-visual\x2dstudio\x2dcode@4.service:2400:2890000000'
  "syncthing.service:96:62000000" "app-krita@5.service:300:271000000")
for u in "${UNITS[@]}"; do IFS=: read -r name mb cpu <<< "$u"; fake "$name" app.slice "$mb" "$cpu" 0.00 40000; done
# CPU time grows for a few rows, so the CPU column and heat shading show real rates.
( while :; do
    for spec in 'app-zen\x2dbrowser@0f1e2d3c.service:340000' 'app-visual\x2dstudio\x2dcode@4.service:120000' app-spotify@2.service:20000; do
      IFS=: read -r name step <<< "$spec"; f=$BASE/app.slice/$name/cpu.stat
      read -r _ now < "$f"; printf 'usage_usec %s\nuser_usec 1\nsystem_usec 1\n' $((now + step)) > "$f"
    done
    sleep 1
  done ) &
export ANDROMEDA_CGROUP_ROOT=$CG REAL_RUNTIME CG POINTER=$ROOT/compositor/build-debug/tests/pointer-client
export SCOPE1=${SCOPES[0]} SCOPE2=${SCOPES[1]} UNITS_LIST="${UNITS[*]}"
MOCK="$(cd "$(dirname "$0")" && pwd)/mock_systemd1.py"
BEFORE_NOCTALIA='REAL_RUNTIME=$REAL_RUNTIME python3 '"$MOCK"' "$OUT/calls.txt" > "$OUT/mock.log" 2>&1 &
  for _ in $(seq 50); do grep -q ready "$OUT/mock.log" && break; sleep 0.1; done'
with_noctalia '
  fail() { echo "FAIL: $*"; exit 1; }
  settle() { "$NOCTALIA" msg status > /dev/null; "$ANDROMEDA" settle > /dev/null; }
  keys() { "$POINTER" 1720 1000 keyboard-only tap 119 pause 100 "$@" > /dev/null; }
  call() { dbus-send --session --print-reply --dest=org.freedesktop.systemd1 /org/freedesktop/systemd1 "org.andromeda.MockSystemd.$@" > /dev/null; }
  "$ANDROMEDA" msg config-reload > /dev/null
  "$NOCTALIA" msg config-set dock.enabled false > /dev/null || true
  for u in $UNITS_LIST; do call Start string:"${u%%:*}" string:app.slice; done
  for s in "$SCOPE1" "$SCOPE2"; do
    XDG_RUNTIME_DIR=$REAL_RUNTIME systemd-run --user --quiet --scope --unit="$s" \
      env XDG_RUNTIME_DIR="$RUNTIME" foot --config=/dev/null --title=Terminal sleep 600 > /dev/null 2>&1 &
    cg=""
    for _ in $(seq 50); do cg=$(XDG_RUNTIME_DIR=$REAL_RUNTIME systemctl --user show -P ControlGroup "$s" 2> /dev/null || true); [[ -n $cg ]] && break; sleep 0.1; done
    [[ -n $cg ]] || fail "scope $s did not start"
    mkdir -p "$(dirname "$CG$cg")"; ln -sfn "/sys/fs/cgroup$cg" "$CG$cg"
    call StartWithCgroup string:"$s" string:app.slice string:"$cg"
  done
  wait_for() { # seconds, message, command...: polls the command every 0.1 s until it passes
    local tries=$(( $1 * 10 )) msg=$2; shift 2
    for _ in $(seq "$tries"); do "$@" > /dev/null 2>&1 && return 0; sleep 0.1; done
    fail "$msg"
  }
  activity_mapped() { "$ANDROMEDA" windows --json | jq -e '"'"'any(.. | objects; .app_id? == "dev.andromeda.Activity")'"'"'; }
  activity_closed() { ! activity_mapped; }
  dump_has() { "$NOCTALIA" msg activity-dump | jq -e "$1"; }
  shot() { # name width row-key keys sorted-header
    printf "\n[[window_rule]]\nmatch.app_id = \"^dev.andromeda.Activity$\"\ndefault_floating = true\ndefault_floating_size_px = { width = %s, height = 720 }\n" "$2" > "$RUNTIME/activity-rule.toml"
    cat "$RUNTIME/andromeda.base.toml" "$RUNTIME/activity-rule.toml" > "$RUNTIME/andromeda.toml"
    "$ANDROMEDA" msg config-reload > /dev/null
    if activity_mapped > /dev/null; then
      keys mod control tap 17 mod none # Ctrl+W: the next open takes the new size rule
      wait_for 5 "Ctrl+W did not close Activity" activity_closed
    fi
    "$NOCTALIA" msg activity-open "$3" > /dev/null
    wait_for 5 "Activity did not map" activity_mapped
    wait_for 10 "no CPU rate for zen-browser" dump_has '"'"'any(.rows[]; .key == "zen-browser" and .cpu_percent > 0)'"'"'
    keys $4
    wait_for 5 "the header does not show $5" dump_has "any(.buttons[]?; .text == \"$5\")"
    settle
    grim "$OUT/$1.png"
  }
  cp "$RUNTIME/andromeda.toml" "$RUNTIME/andromeda.base.toml"
  "$NOCTALIA" msg theme-mode-set light > /dev/null
  shot light-memory-1100 1100 "" "mod control tap 3 mod none" "Memory ▾"
  shot light-memory-1600 1600 "" "mod control tap 3 mod none" "Memory ▾"
  printf "some avg10=0.00 avg60=0.00 avg300=0.00 total=1\n" > "$CG/user.slice/user-1000.slice/user@1000.service/app.slice/app-zen\\x2dbrowser@0f1e2d3c.service/memory.pressure"
  "$NOCTALIA" msg theme-mode-set dark > /dev/null
  shot dark-cpu-1100 1100 zen-browser "mod control tap 2 mod none" "CPU ▾"
  shot dark-cpu-1600 1600 zen-browser "mod control tap 2 mod none" "CPU ▾"
  "$NOCTALIA" msg activity-dump > "$OUT/dump.json"
'
echo "PASS: shots in $OUT"
