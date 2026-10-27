#!/usr/bin/env bash
# harness: outputs=2
# Floating and pinned windows must return to exact full-output-relative
# positions when the output comes back before its exclusive-zone panel.
set -euo pipefail

readonly LAYER_CLIENT="${ANDROMEDA_LAYER_CLIENT:-./build-debug/tests/layer-client}"
readonly WINDOW_CLIENT="${ANDROMEDA_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly LAYER_HEIGHT=96

cat >> "$ANDROMEDA_CONFIG" <<'EOF'

[animation]
enabled = false

[output.HEADLESS-1]
mode = "2560x1440"
position = [0, 0]

[output.HEADLESS-2]
mode = "1920x1080"
position = [2560, 0]

[[window_rule]]
match.title = "^restore-(float|pin)$"
default_output = "HEADLESS-1"
default_floating = true
default_floating_size_px = { width = 500, height = 300 }
default_position = { x = 2000, y = 1000, anchor = "top_left" }
EOF
"$ANDROMEDA" msg config-reload > /dev/null

start_panel() {
  local log=$1
  "$LAYER_CLIENT" HEADLESS-1 "$LAYER_HEIGHT" > "$log" 2>&1 &
  PANEL_PID=$!
  for _ in $(seq 80); do
    grep -q '^ready$' "$log" && return 0
    if ! kill -0 "$PANEL_PID" 2>/dev/null; then
      echo "exclusive-zone panel exited before mapping: $(< "$log")"
      return 1
    fi
    sleep 0.1
  done
  echo "exclusive-zone panel did not map: $(< "$log")"
  return 1
}

spawn_client() {
  "$WINDOW_CLIENT" "$1" 500 300 > "$ANDROMEDA_RUNTIME_DIR/$1.log" 2>&1 &
}

wait_for_field() {
  local title=$1 field=$2 expected=$3 actual=
  for _ in $(seq 80); do
    actual=$(field_of "$title" "$field")
    [[ $actual == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected '$title' field '$field' to be '$expected', got '$actual'"
  return 1
}

wait_for_count() {
  local expected=$1 count=
  for _ in $(seq 80); do
    count=$("$ANDROMEDA" windows --json | jq 'length')
    [[ $count == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected $expected windows, got $count"
  return 1
}

field_of() {
  "$ANDROMEDA" windows --json \
    | jq -r --arg title "$1" --arg field "$2" '.[] | select(.title == $title) | .[$field]'
}

wait_for_output() {
  local title=$1 output=$2 workspace=
  for _ in $(seq 80); do
    workspace=$(field_of "$title" workspace)
    [[ $workspace == "$output":* ]] && return 0
    sleep 0.1
  done
  echo "expected '$title' on '$output', got workspace '$workspace'"
  return 1
}

geometry_snapshot() {
  "$ANDROMEDA" windows --json \
    | jq -Sc '[.[] | select(.title | test("^restore-(float|pin)$")) | {title, x, y, w, h, floating}] | sort_by(.title)'
}

wait_for_stable_snapshot() {
  local previous= current= stable=0
  for _ in $(seq 80); do
    current=$(geometry_snapshot)
    if [[ -n $current && $current == "$previous" ]]; then
      stable=$((stable + 1))
      if ((stable >= 3)); then
        printf '%s' "$current"
        return 0
      fi
    else
      stable=0
    fi
    previous=$current
    sleep 0.1
  done
  echo "floating geometry did not settle: $current" >&2
  return 1
}

panel_log="$ANDROMEDA_RUNTIME_DIR/restore-panel-first.log"
start_panel "$panel_log"

spawn_client restore-float
wait_for_count 1
spawn_client restore-pin
wait_for_count 2
for title in restore-float restore-pin; do
  wait_for_output "$title" HEADLESS-1
  wait_for_field "$title" floating true
  "$ANDROMEDA" msg "window-focus-warp:$(field_of "$title" id)" > /dev/null
  "$ANDROMEDA" msg window-center > /dev/null
  wait_for_field "$title" x 1030
  wait_for_field "$title" y 618
done
"$ANDROMEDA" msg "window-focus-warp:$(field_of restore-pin id)" > /dev/null
"$ANDROMEDA" msg window-toggle-pinned > /dev/null
before=$(wait_for_stable_snapshot)

"$ANDROMEDA" output-destroy HEADLESS-1 > /dev/null
for title in restore-float restore-pin; do
  wait_for_output "$title" HEADLESS-2
done
if ! wait "$PANEL_PID"; then
  echo "first exclusive-zone panel failed while its output closed: $(< "$panel_log")"
  exit 1
fi

created=$("$ANDROMEDA" output-create HEADLESS-1)
if [[ $created != HEADLESS-1 ]]; then
  echo "expected recreated output HEADLESS-1, got '$created'"
  exit 1
fi
for title in restore-float restore-pin; do
  wait_for_output "$title" HEADLESS-1
done

# Match Noctalia's real ordering: its panel maps just after Andromeda has already
# restored displaced windows onto the newly returned output.
panel_log="$ANDROMEDA_RUNTIME_DIR/restore-panel-second.log"
start_panel "$panel_log"
after=$(wait_for_stable_snapshot)
if [[ $after != "$before" ]]; then
  echo "floating geometry changed when the panel returned after the output"
  echo "before: $before"
  echo " after: $after"
  exit 1
fi

echo "floating and pinned positions survived a smaller refuge and a late returning panel"
