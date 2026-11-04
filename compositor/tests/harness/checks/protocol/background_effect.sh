#!/usr/bin/env bash
# ext_background_effect_manager_v1 is offered, a layer surface's blur region turns its blur on with no layer rule, and
# a NULL region turns it off again on the next commit.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly GLOBAL_CLIENT="${ANDROMEDA_GLOBAL_CLIENT:-./build-debug/tests/global-client}"
readonly LAYER_CLIENT="${ANDROMEDA_LAYER_CLIENT:-./build-debug/tests/layer-client}"
readonly LOG="$ANDROMEDA_RUNTIME_DIR/background-effect.log"
readonly FIFO="$ANDROMEDA_RUNTIME_DIR/background-effect.fifo"

if [[ ! -x $GLOBAL_CLIENT || ! -x $LAYER_CLIENT ]]; then
  echo "required harness clients are not built"
  exit 1
fi

"$GLOBAL_CLIENT" ext_background_effect_manager_v1 present 1

await_blur() {
  local expected=$1 reason=$2 state=
  for _ in $(seq 60); do
    state=$("$ANDROMEDA" layers --json)
    if jq -e --argjson want "$expected" 'length == 1 and .[0].mapped and .[0].blur == $want' <<< "$state" > /dev/null; then
      return 0
    fi
    sleep 0.05
  done
  echo "$reason: $state"
  exit 1
}

mkfifo "$FIFO"
exec {control_fd}<> "$FIFO"
LAYER_CONTROL=1 "$LAYER_CLIENT" HEADLESS-1 40 <&"$control_fd" > "$LOG" 2>&1 &
client_pid=$!
trap 'kill "$client_pid" 2> /dev/null || true' EXIT

# The client takes one command at a time, after its first frame ("ready") and after each "done <label>".
await_lines "$LOG" ready 1
await_blur false "the layer surface did not map without blur"
printf '%s\n' "blur-region on 0 0 200 20" >&"$control_fd"
await_lines "$LOG" 'done on ' 1
await_blur true "a blur region did not turn blur on"
printf '%s\n' "blur-none off" >&"$control_fd"
await_lines "$LOG" 'done off ' 1
await_blur false "a NULL blur region did not turn blur off"
