#!/usr/bin/env bash
set -euo pipefail

if [[ "${EUID}" -ne 0 ]]; then
  echo "error: run as root (use sudo)." >&2
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=greetd_setup_lib.sh
source "${SCRIPT_DIR}/greetd_setup_lib.sh"

SESSION_BIN="$(find_session_bin)"
SYNCED_DATA_DIR="/var/lib/noctalia-greeter"

APPLY_APPEARANCE="$(find_apply_appearance || true)"
GREETER_USER="$(resolve_greeter_user)"

echo "info: applying greetd PAM runtime module patch..."
"${SCRIPT_DIR}/setup_greetd_pam.sh"

echo "info: preparing greeter paths..."
ensure_greeter_paths "${GREETER_USER}"

# Optional: also apply systemd/opentmpfiles drop-in when present (boot recreate).
# Portable ensure_greeter_paths above remains the source of truth for packaging.
if command -v systemd-tmpfiles >/dev/null 2>&1; then
  systemd-tmpfiles --create noctalia-greeter.conf 2>/dev/null || true
elif command -v opentmpfiles >/dev/null 2>&1; then
  opentmpfiles --create noctalia-greeter.conf 2>/dev/null || true
elif command -v tmpfiles >/dev/null 2>&1; then
  tmpfiles --create noctalia-greeter.conf 2>/dev/null || true
fi

if [[ -n "${APPLY_APPEARANCE}" ]]; then
  echo "info: installing greeter.toml via ${APPLY_APPEARANCE} --setup-system"
  GREETER_USER="${GREETER_USER}" "${APPLY_APPEARANCE}" --setup-system
else
  echo "error: noctalia-greeter-apply-appearance not found; build/install first." >&2
  exit 1
fi

echo "info: appearance sync target: ${SYNCED_DATA_DIR}"

if [[ ! -x "${SESSION_BIN}" ]]; then
  echo "warn: session launcher '${SESSION_BIN}' not found or not executable."
  echo "warn: install first via: sudo meson install -C build"
fi

echo
echo "System setup complete."
echo
print_greetd_config_commands "${SESSION_BIN}" "${GREETER_USER}"
