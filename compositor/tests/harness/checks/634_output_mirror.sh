#!/usr/bin/env bash
# harness: outputs=2
# `mirror` makes an output leave the desktop and show another output's frames. Its window moves to the source, the
# mirror stops being a wl_output clients can address, and removing the key brings the output and its window back.
# The mirror's own pixels need a real renderer on a visible output; tests/unit/mirror_placement.cpp pins the geometry.
set -euo pipefail

BASELINE=$(< "$UMBRIEL_CONFIG")
readonly SOURCE_SHOT="$UMBRIEL_RUNTIME_DIR/mirror-source.png"
readonly TARGET_SHOT="$UMBRIEL_RUNTIME_DIR/mirror-target.png"
readonly AFTER_SHOT="$UMBRIEL_RUNTIME_DIR/mirror-after.png"
readonly GREEN='g > 0.9 && r < 0.1 && b < 0.1'

log_mark() { wc -l < "$UMBRIEL_LOG"; }
wait_for_log_since() {
  local mark=$1 pattern=$2
  for _ in $(seq 40); do
    tail -n +"$((mark + 1))" "$UMBRIEL_LOG" | grep -q "$pattern" && return 0
    sleep 0.25
  done
  return 1
}
wait_for_workspace() {
  local expected=$1 workspace=
  for _ in $(seq 40); do
    workspace=$("$UMBRIEL" windows --json | jq -r '.[0].workspace // empty')
    [[ $workspace == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected window workspace '$expected', got '$workspace'"
  return 1
}

foot --config=/dev/null --override=colors-dark.background=00ff00 --title=mirror-green sh -c 'sleep 120' > /dev/null 2>&1 &
for _ in $(seq 40); do
  [[ $("$UMBRIEL" windows --json | jq 'length') -eq 1 ]] && break
  sleep 0.1
done

mark=$(log_mark)
{
  printf '%s\n' "$BASELINE"
  printf '\n[output.HEADLESS-2]\nmirror = "HEADLESS-1"\n'
} > "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
if ! wait_for_log_since "$mark" "output 'HEADLESS-2': mirroring 'HEADLESS-1'"; then
  echo "HEADLESS-2 did not start mirroring on reload"
  tail -8 "$UMBRIEL_LOG" | sed 's/^/  | /'
  exit 1
fi
wait_for_workspace 'HEADLESS-1:1'
"$UMBRIEL" settle > /dev/null
grim -o HEADLESS-1 "$SOURCE_SHOT"
source_green=$("$UMBRIEL_PIXEL_PROBE" "$SOURCE_SHOT" count "$GREEN")
if (( source_green < 10000 )); then
  echo "the source shows too little of the green window: $source_green pixels"
  exit 1
fi
if grim -o HEADLESS-2 "$TARGET_SHOT" 2> /dev/null; then
  echo "the mirroring output is still a wl_output clients can capture or place surfaces on"
  exit 1
fi

target_commits() { "$UMBRIEL" output-commits --json | jq -r '."HEADLESS-2" // .ok."HEADLESS-2"'; }
before=$(target_commits)
sleep 1 # real time: a static source must not make the mirror redraw at its refresh rate
idle_commits=$(($(target_commits) - before))
if (( idle_commits > 2 )); then
  echo "the mirror redrew $idle_commits times in 1 s while its source was static"
  exit 1
fi
before=$(target_commits)
foot --config=/dev/null --title=mirror-change sh -c 'sleep 120' > /dev/null 2>&1 &
change_pid=$!
for _ in $(seq 40); do
  (( $(target_commits) > before )) && break
  sleep 0.1
done
if (( $(target_commits) <= before )); then
  echo "the mirror did not redraw when its source changed"
  exit 1
fi
kill "$change_pid"
for _ in $(seq 40); do
  [[ $("$UMBRIEL" windows --json | jq 'length') -eq 1 ]] && break
  sleep 0.1
done

mark=$(log_mark)
printf '%s\n' "$BASELINE" > "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
if ! wait_for_log_since "$mark" "output 'HEADLESS-2': stopped mirroring"; then
  echo "HEADLESS-2 kept mirroring after the key was removed"
  exit 1
fi
wait_for_workspace 'HEADLESS-2:1'
"$UMBRIEL" settle > /dev/null
grim -o HEADLESS-2 "$AFTER_SHOT"
after_green=$("$UMBRIEL_PIXEL_PROBE" "$AFTER_SHOT" count "$GREEN")
if (( after_green < 10000 )); then
  echo "HEADLESS-2 rejoined the desktop but does not show its restored window: $after_green green pixels"
  exit 1
fi

echo "mirroring moves the window to the source and hides the output from clients ($idle_commits idle mirror commits in 1 s); removing it restores both"
