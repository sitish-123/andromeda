#!/usr/bin/env bash
# xdg_wm_dialog_v1 is offered; a child that marks itself modal is reported modal over IPC, and focusing its parent
# focuses the dialog instead.
set -euo pipefail
source "$ANDROMEDA_HARNESS_LIB"

readonly CLIENT="${ANDROMEDA_SURFACE_PROTOCOLS_CLIENT:-./build-debug/tests/surface-protocols-client}"
readonly WINDOW_CLIENT="${ANDROMEDA_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly LOG="$ANDROMEDA_RUNTIME_DIR/dialog.log"

"$CLIENT" dialog editor > "$LOG" 2>&1 &
await_events "$LOG" modal 1 "the modal dialog"
"$WINDOW_CLIENT" bystander > "$ANDROMEDA_RUNTIME_DIR/bystander.log" 2>&1 &
await_events "$ANDROMEDA_RUNTIME_DIR/bystander.log" mapped 1 "the bystander window"
"$ANDROMEDA" settle

window() { "$ANDROMEDA" windows --json | jq -r --arg title "$1" ".[] | select(.title == \$title) | .$2"; }
if [[ $(window editor-modal modal) != true || $(window editor modal) != false ]]; then
  echo "IPC does not report the dialog modal: $("$ANDROMEDA" windows --json | jq -c '[.[] | {title, modal}]')"
  exit 1
fi

"$ANDROMEDA" msg "window-focus:$(window bystander id)" > /dev/null
"$ANDROMEDA" settle
[[ $(window bystander active) == true ]] || { echo "the bystander never took focus"; exit 1; }
"$ANDROMEDA" msg "window-focus:$(window editor id)" > /dev/null
"$ANDROMEDA" settle
if [[ $(window editor-modal active) != true ]]; then
  echo "focusing the parent did not focus its modal dialog: $("$ANDROMEDA" windows --json | jq -c '[.[] | {title, active}]')"
  exit 1
fi
echo "modal dialog reported over IPC and given its parent's focus"
