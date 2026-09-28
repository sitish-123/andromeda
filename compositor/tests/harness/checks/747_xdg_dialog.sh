#!/usr/bin/env bash
# xdg_wm_dialog_v1 is offered; a child that marks itself modal is reported modal over IPC, and focusing its parent
# focuses the dialog instead.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly CLIENT="${UMBRIEL_SURFACE_PROTOCOLS_CLIENT:-./build-debug/tests/surface-protocols-client}"
readonly WINDOW_CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly LOG="$UMBRIEL_RUNTIME_DIR/dialog.log"

"$CLIENT" dialog editor > "$LOG" 2>&1 &
await_events "$LOG" modal 1 "the modal dialog"
"$WINDOW_CLIENT" bystander > "$UMBRIEL_RUNTIME_DIR/bystander.log" 2>&1 &
await_events "$UMBRIEL_RUNTIME_DIR/bystander.log" mapped 1 "the bystander window"
"$UMBRIEL" settle

window() { "$UMBRIEL" windows --json | jq -r --arg title "$1" ".[] | select(.title == \$title) | .$2"; }
if [[ $(window editor-modal modal) != true || $(window editor modal) != false ]]; then
  echo "IPC does not report the dialog modal: $("$UMBRIEL" windows --json | jq -c '[.[] | {title, modal}]')"
  exit 1
fi

"$UMBRIEL" msg "window-focus:$(window bystander id)" > /dev/null
"$UMBRIEL" settle
[[ $(window bystander active) == true ]] || { echo "the bystander never took focus"; exit 1; }
"$UMBRIEL" msg "window-focus:$(window editor id)" > /dev/null
"$UMBRIEL" settle
if [[ $(window editor-modal active) != true ]]; then
  echo "focusing the parent did not focus its modal dialog: $("$UMBRIEL" windows --json | jq -c '[.[] | {title, active}]')"
  exit 1
fi
echo "modal dialog reported over IPC and given its parent's focus"
