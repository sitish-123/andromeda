#!/usr/bin/env bash
# A crashed compositor restarts on the socket-activated listening socket; clients keep WAYLAND_DISPLAY (R1).
set -euo pipefail
python3 "$ANDROMEDA_REPO/tests/session_restart.py" "$ANDROMEDA"
