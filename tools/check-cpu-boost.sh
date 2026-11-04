#!/usr/bin/env bash
# Prints each change of CPUWeight on app-*.service units for $1 seconds (default 60), live session only.
# Expected (TASK-boost.md): a launch shows 1000, then 400 (focused) or 100; a focus change swaps 400 and 100.
set -euo pipefail
end=$((SECONDS + ${1:-60}))
declare -A last
while ((SECONDS < end)); do
  while read -r unit; do
    w=$(systemctl --user show "$unit" -p CPUWeight --value 2>/dev/null || true)
    if [[ -n $w && ${last[$unit]:-} != "$w" ]]; then
      printf '%(%T)T %s CPUWeight=%s\n' -1 "$unit" "$w"
      last[$unit]=$w
    fi
  done < <(systemctl --user list-units --plain --no-legend 'app-*.service' | awk '{print $1}')
  sleep 0.2
done
