#!/usr/bin/env bash
# F2 in the overview renames the shown workspace: typing replaces the name, Enter keeps it, Esc drops the edit, and
# a blank name keeps the old one. The name is a label for this session; the configured name stays (OV8).
set -euo pipefail

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly POINTER="${ANDROMEDA_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
# evdev key codes
readonly K_ESC=1 K_BACKSPACE=14 K_ENTER=28 K_F2=60 K_A=30 K_B=48 K_X=45

keys() {
  "$POINTER" "$OUTPUT_W" "$OUTPUT_H" "$@"
  "$ANDROMEDA" settle
}
field() { "$ANDROMEDA" workspaces --json | jq -r ".[] | select(.active) | .$1"; }

cat >> "$ANDROMEDA_CONFIG" <<'EOF2'

[overview]
style = "spaces"

[animation]
enabled = false
EOF2
"$ANDROMEDA" msg config-reload > /dev/null
"$ANDROMEDA_UNMAP_CLIENT" rename-a 400 300 > /dev/null 2>&1 &
for _ in $(seq 60); do
  [[ $("$ANDROMEDA" windows --json | jq 'length') -eq 1 ]] && break
  sleep 0.05
done
"$ANDROMEDA" msg overview-open > /dev/null
"$ANDROMEDA" settle
name=$(field name)

keys tap $K_F2 tap $K_A tap $K_B tap $K_ENTER
[[ $(field label) == ab ]] || { echo "F2, a, b, Enter labelled the workspace '$(field label)'"; exit 1; }
[[ $(field name) == "$name" ]] || { echo "a rename changed the configured name to '$(field name)'"; exit 1; }

keys tap $K_F2 tap $K_X tap $K_ESC
[[ $(field label) == ab ]] || { echo "Esc kept the edit: '$(field label)'"; exit 1; }
keys tap $K_F2 tap $K_BACKSPACE tap $K_ENTER
[[ $(field label) == ab ]] || { echo "a blank name replaced the label: '$(field label)'"; exit 1; }
echo "F2 renames the shown workspace; Esc and a blank name keep the old one"
