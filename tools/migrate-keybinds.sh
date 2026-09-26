#!/usr/bin/env bash
# Usage: migrate-keybinds.sh [config=~/.config/umbriel/config.toml]
# Rewrites `spawn:noctalia msg <cmd>` keybind actions to `shell:<cmd>`, which reach the shell over dsk_shell_v1
# without starting a process. Keeps a backup next to the config. Refuses to run until the running compositor knows
# the shell action, since an older one would reject the rewritten config.
set -euo pipefail

config=${1:-${XDG_CONFIG_HOME:-$HOME/.config}/umbriel/config.toml}
if umbriel msg 'shell:status' 2>&1 | grep -qi 'unknown action'; then
  echo "the running compositor has no shell: action yet; install the new package and log in again first" >&2
  exit 1
fi
count=$(grep -c 'spawn:noctalia msg ' "$config" || true)
if ((count == 0)); then
  echo "nothing to migrate in $config"
  exit 0
fi
backup="$config.bak-keybinds-$(date +%Y%m%d-%H%M%S)"
cp "$config" "$backup"
sed -i 's/spawn:noctalia msg /shell:/g' "$config"
echo "migrated $count keybinds in $config (backup: $backup)"
