#!/usr/bin/env bash
# The shell serves org.freedesktop.impl.portal.GlobalShortcuts: an app session binds a shortcut, `noctalia msg
# global-shortcuts` lists it, and `noctalia msg global-shortcut <app-id> <id>` (what a compositor keybind
# `shell:global-shortcut …` runs) emits Activated then Deactivated for that session; an unbound id is refused.
# Writes list.txt and signals.txt to $OUT (default ./artifacts/global-shortcuts).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/global-shortcuts}
source "$(dirname "$0")/lib.sh"
boot_headless 1

with_noctalia '
  portal() { gdbus call --session --dest org.freedesktop.impl.portal.desktop.noctalia \
    --object-path /org/freedesktop/portal/desktop --method org.freedesktop.impl.portal.GlobalShortcuts.$1 "${@:2}"; }
  session=/org/freedesktop/portal/desktop/session/1_1/e2e
  portal CreateSession /org/freedesktop/portal/desktop/request/1_1/c "$session" e2e.app "{}" > /dev/null
  bound=$(portal BindShortcuts /org/freedesktop/portal/desktop/request/1_1/b "$session" \
    "[(\"toggle\", {\"description\": <\"Toggle the thing\">})]" "" "{}")
  [[ $bound == *"toggle"*"shell:global-shortcut e2e.app toggle"* ]] || { echo "FAIL: BindShortcuts answered $bound"; exit 1; }

  "$NOCTALIA" msg global-shortcuts > "$OUT/list.txt"
  gdbus monitor --session --dest org.freedesktop.impl.portal.desktop.noctalia > "$OUT/signals.txt" &
  sleep 0.3
  [[ $("$NOCTALIA" msg global-shortcut e2e.app toggle) == ok ]] || { echo "FAIL: activation refused"; exit 1; }
  refused=$("$NOCTALIA" msg global-shortcut e2e.app missing || true)
  [[ $refused == error* ]] || { echo "FAIL: unbound id accepted: $refused"; exit 1; }
  for _ in $(seq 30); do grep -q Deactivated "$OUT/signals.txt" && break; sleep 0.1; done
  kill %2
'
cat "$OUT/list.txt"
grep -qx "e2e.app toggle Toggle the thing" "$OUT/list.txt" || { echo "FAIL: shortcut not listed"; exit 1; }
for signal in Activated Deactivated; do
  grep "GlobalShortcuts.$signal " "$OUT/signals.txt" | grep -q toggle || { echo "FAIL: no $signal"; exit 1; }
done
echo "PASS; artifacts: $OUT"
