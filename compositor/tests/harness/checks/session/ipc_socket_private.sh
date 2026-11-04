#!/usr/bin/env bash
# The IPC socket is owner-only (0600), so other users on the machine cannot connect to it.
set -euo pipefail
mode=$(stat -c %a "$ANDROMEDA_SOCKET")
[[ $mode == 600 ]] || { echo "IPC socket mode is $mode, want 600"; exit 1; }
"$ANDROMEDA" msg config-reload > /dev/null
