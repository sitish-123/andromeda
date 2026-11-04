#!/usr/bin/env bash
# A crash while locked restarts into a blank lock; only a lock client unlocks, and no lock client ends the session (R3).
set -euo pipefail
python3 "$ANDROMEDA_REPO/tests/session_restart.py" "$ANDROMEDA" lock
