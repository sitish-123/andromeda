#!/usr/bin/env bash
# A log-out stops the apps before the compositor; session.json keeps the window list from before the log-out (R6).
set -euo pipefail
python3 "$ANDROMEDA_REPO/tests/session_logout.py" "$ANDROMEDA"
