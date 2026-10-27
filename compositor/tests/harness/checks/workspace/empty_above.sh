#!/usr/bin/env bash
# empty_above keeps distinct leading and trailing empty workspaces. Moving the active leading empty workspace must
# preserve that inventory and renumber every surviving workspace.
set -euo pipefail

readonly WORKSPACE="${ANDROMEDA_WORKSPACE_CLIENT:-./build-debug/tests/workspace-client}"

workspace_names() {
  "$WORKSPACE" --all | cut -f2 | sort -n | paste -sd ' ' -
}

workspace_name_for_window() {
  local workspace_id
  workspace_id=$("$ANDROMEDA" windows --json | jq -r '.[0].workspace')
  "$WORKSPACE" --all | awk -F'\t' -v id="$workspace_id" '$1 == id { print $2 }'
}

printf '\n[workspaces]\nempty_above = true\n' >> "$ANDROMEDA_CONFIG"
"$ANDROMEDA" msg config-reload > /dev/null

foot --title=empty-above-view sh -c 'sleep 120' > /dev/null 2>&1 &
for _ in $(seq 40); do
  [[ $("$ANDROMEDA" windows --json | jq 'length') -eq 1 ]] && break
  sleep 0.1
done
if [[ $("$ANDROMEDA" windows --json | jq 'length') -ne 1 ]]; then
  echo "expected one window, got: $("$ANDROMEDA" windows --json)"
  exit 1
fi
if [[ $(workspace_names) != "1 2 3" || $(workspace_name_for_window) != "2" ]]; then
  echo "mapped view did not land between both empty sentinels: names=$(workspace_names), view=$(workspace_name_for_window)"
  exit 1
fi

"$ANDROMEDA" msg workspace-switch:1 > /dev/null
"$ANDROMEDA" msg workspace-move-down > /dev/null

if [[ $(workspace_names) != "1 2 3" ]]; then
  echo "moving the leading empty workspace left stale or duplicate names: $(workspace_names)"
  exit 1
fi
if [[ $("$WORKSPACE") != "3" || $(workspace_name_for_window) != "2" ]]; then
  echo "workspace identities were not renumbered after the move: active=$("$WORKSPACE"), view=$(workspace_name_for_window)"
  exit 1
fi

echo "empty_above keeps distinct sentinels and renumbers after workspace movement"
