#!/usr/bin/env bash
# The Containers tab. Three engine sockets, all owned by this user (so their units are user units):
#   docker.sock  mock_docker.py with web and cache running and build exited, the list sent chunked; the running ones
#                report a busy loop's PID, so usage comes from its real cgroup; mock_systemd1.py has docker.service.
#   podman.sock  a second mock_docker.py; only podman.socket runs, no podman.service (socket activation).
#   denied.sock  a socket file with mode 000.
# All actions are real pointer clicks. Fails when:
#   - the dump lacks the three containers, or web has no memory or no CPU,
#   - the denied engine does not report "permission", or the podman engine is not idle,
#   - anything asks the idle podman socket (that request would start the daemon),
#   - Stop Engine on podman does not stop podman.socket,
#   - Pause, Resume or Stop in the inspector does not reach the engine (pause / unpause / stop web),
#   - Stop Engine does not stop docker.service, or Don't Start at Login does not disable it.
# Writes containers.png and inspector.png to $OUT (default ./artifacts/activity-containers).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/activity-containers}
source "$(dirname "$0")/lib.sh"
boot_headless 1
E2E=$(cd "$(dirname "$0")" && pwd)
bash -c 'while :; do :; done' &
HOLDER=$!
trap 'kill $HOLDER 2> /dev/null || true' EXIT
mkdir -p "$RUNTIME/podman"
python3 -c 'import os, socket, sys; s = socket.socket(socket.AF_UNIX); s.bind(sys.argv[1]); os.chmod(sys.argv[1], 0)' "$RUNTIME/denied.sock"
export NOCTALIA_CONTAINER_SOCKETS=$RUNTIME/docker.sock:$RUNTIME/podman/podman.sock:$RUNTIME/denied.sock HOLDER E2E POINTER=$ROOT/compositor/build-debug/tests/pointer-client
: > "$OUT/calls.txt"
rm -f "$OUT/result.txt"
: > "$OUT/docker-calls.txt"
: > "$OUT/podman-calls.txt"
BEFORE_NOCTALIA='python3 "$E2E/mock_systemd1.py" "$OUT/calls.txt" > "$OUT/mock.log" 2>&1 &
  python3 "$E2E/mock_docker.py" "$RUNTIME/docker.sock" "$OUT/docker-calls.txt" "$HOLDER" --chunked > "$OUT/docker.log" 2>&1 &
  python3 "$E2E/mock_docker.py" "$RUNTIME/podman/podman.sock" "$OUT/podman-calls.txt" "$HOLDER" > "$OUT/podman.log" 2>&1 &
  for _ in $(seq 50); do grep -q ready "$OUT/mock.log" && grep -q ready "$OUT/docker.log" && grep -q ready "$OUT/podman.log" && break; sleep 0.1; done'
with_noctalia '
  click() { bash "$E2E/activity_click.sh" "$@"; }
  wait_for() { # jq filter on the dump, message
    for _ in $(seq 60); do "$NOCTALIA" msg activity-dump > "$OUT/dump.json"; jq -e "$1" "$OUT/dump.json" > /dev/null && return 0; sleep 0.1; done
    echo "FAIL: $2: $(jq -c "{containers: [.containers[]? | {name, state, memory_bytes}], buttons: [.buttons[]?.text]}" "$OUT/dump.json")"; exit 1
  }
  calls() { for _ in $(seq 50); do grep -qx "$1" "$2" && return 0; sleep 0.1; done; echo "FAIL: never received: $1"; exit 1; }
  for unit in docker.service podman.socket; do
    dbus-send --session --print-reply --dest=org.freedesktop.systemd1 /org/freedesktop/systemd1 \
      org.andromeda.MockSystemd.Start string:$unit string:app.slice > /dev/null
  done

  "$NOCTALIA" msg activity-open containers > /dev/null
  wait_for "[.containers[]? | .name] | sort == [\"build\", \"cache\", \"web\"]" "the three containers are not in the dump"
  wait_for "any(.containers[]; .name == \"web\" and .memory_bytes > 0 and .cpu_percent > 0)" "web has no memory or CPU from its cgroup"
  wait_for "any(.engines[]; (.socket | endswith(\"denied.sock\")) and .error == \"permission\")" "the mode-000 engine does not report permission"
  wait_for "any(.engines[]; (.socket | endswith(\"podman.sock\")) and .idle)" "the socket-only podman engine is not idle"
  wait_for "any(.buttons[]?; .text == \"Stop Engine\") and any(.buttons[]?; .text == \"Don'"'"'t Start at Login\")" "no engine buttons"
  grim "$OUT/containers.png"

  click web
  wait_for "any(.buttons[]?; .text == \"Pause\")" "selecting web shows no Pause"
  grim "$OUT/inspector.png"
  click Pause;  calls "pause web" "$OUT/docker-calls.txt"
  wait_for "any(.buttons[]?; .text == \"Resume\")" "a paused web shows no Resume"
  click Resume; calls "unpause web" "$OUT/docker-calls.txt"
  wait_for "any(.buttons[]?; .text == \"Pause\")" "a resumed web shows no Pause"
  click Stop;   calls "stop web" "$OUT/docker-calls.txt"
  wait_for "any(.containers[]; .name == \"web\" and .state == \"exited\")" "web did not show as exited"

  # Checked before its socket stops: a stopped engine is no longer idle and may be asked (nothing listens then).
  [[ ! -s $OUT/podman-calls.txt ]] || { echo "FAIL: the idle podman socket was asked: $(head -3 "$OUT/podman-calls.txt")"; exit 1; }
  click "Stop Engine" 1;           calls "StopUnit podman.socket replace" "$OUT/calls.txt"
  click "Don'"'"'t Start at Login"; calls "DisableUnitFiles docker.service False" "$OUT/calls.txt"
  click "Stop Engine";             calls "StopUnit docker.service replace" "$OUT/calls.txt"
  echo "result=ok" > "$OUT/result.txt"
'
grep -qx "result=ok" "$OUT/result.txt" 2> /dev/null || { echo "FAIL: see above" >&2; exit 1; }
echo "PASS: containers listed (chunked) with cgroup usage; denied and idle engines reported, idle socket never asked; Stop Engine stopped podman.socket; Pause, Resume, Stop, Don't Start at Login and Stop Engine reached the engine and systemd; artifacts: $OUT"
