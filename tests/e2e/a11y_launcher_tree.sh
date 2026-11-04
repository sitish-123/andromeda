#!/usr/bin/env bash
# A screen reader sees the launcher: with AT-SPI enabled, the shell publishes an "andromeda-shell" application whose
# tree holds the search field and a selected list item named "foot" after typing "foot". Fails when the app, the
# entry or the row is missing, or when a Firefox-style client (pyatspi) cannot read names, roles and states.
# DoAction on that row must open a foot window. Writes atspi-tree.txt, unnamed.txt and noctalia.log to $OUT (default ./artifacts/a11y-launcher-tree).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/a11y-launcher-tree}
E2E_DIR=$(cd "$(dirname "$0")" && pwd)
export E2E_DIR
source "$E2E_DIR/lib.sh"
boot_headless 1

# The private bus has no service files, and at-spi-bus-launcher needs systemd socket activation: start an
# accessibility bus (it activates at-spi2-registryd) and a stand-in org.a11y.Bus that reports AT-SPI enabled.
BEFORE_NOCTALIA='
  export DESK_A11Y_STRICT=1
  dbus-daemon --config-file=/usr/share/defaults/at-spi2/accessibility.conf --nofork --print-address=3 \
    3> "$RUNTIME/a11y-address" > "$OUT/at-spi.log" 2>&1 &
  for _ in $(seq 100); do [[ -s $RUNTIME/a11y-address ]] && break; sleep 0.05; done
  python3 -I "$E2E_DIR/a11y_bus_mock.py" "$(head -n1 "$RUNTIME/a11y-address")" >> "$OUT/at-spi.log" 2>&1 &
  for _ in $(seq 100); do gdbus call --session -d org.a11y.Bus -o /org/a11y/bus -m org.a11y.Bus.GetAddress > /dev/null 2>&1 && break; sleep 0.05; done
' with_noctalia '
  wait_for() { for _ in $(seq 100); do eval "$2" && return 0; sleep 0.1; done; echo "FAIL: $1"; exit 1; }
  wait_for "the shell did not publish its tree" "grep -q \"accessibility tree published\" \"$OUT/noctalia.log\""
  "$NOCTALIA" msg panel-open launcher foot > /dev/null
  wait_for "the launcher did not draw" "grep -q \"open to first frame\" \"$OUT/noctalia.log\""
  python3 -I - "$OUT/atspi-tree.txt" <<PY
import sys, time
import gi
gi.require_version("Atspi", "2.0")
from gi.repository import Atspi

def walk(node, depth, out):
    role = node.get_role_name()
    name = node.get_name() or ""
    states = node.get_state_set()
    out.append((node, depth, role, name, states.contains(Atspi.StateType.SELECTED), states.contains(Atspi.StateType.SHOWING)))
    for i in range(node.get_child_count()):
        child = node.get_child_at_index(i)
        if child is not None:
            walk(child, depth + 1, out)

deadline = time.monotonic() + 10
while True:
    desktop = Atspi.get_desktop(0)
    apps = [desktop.get_child_at_index(i) for i in range(desktop.get_child_count())]
    shell = next((a for a in apps if a is not None and a.get_name() == "andromeda-shell"), None)
    rows = []
    if shell is not None:
        walk(shell, 0, rows)
    entry = any(r[2] == "entry" and r[5] for r in rows)
    item = next((r[0] for r in rows if r[2] == "list item" and r[3].lower() == "foot" and r[4]), None)
    if (entry and item is not None) or time.monotonic() > deadline:
        break
    time.sleep(0.2)

with open(sys.argv[1], "w") as f:
    for _, depth, role, name, selected, showing in rows:
        flags = ("selected " if selected else "") + ("" if showing else "hidden")
        f.write("  " * depth + role + " " + repr(name) + " " + flags + "\n")
if shell is None:
    sys.exit("FAIL: no andromeda-shell application on the AT-SPI desktop")
if not entry:
    sys.exit("FAIL: no showing entry (search field) in the shell tree")
if item is None:
    sys.exit("FAIL: no selected list item named foot in the shell tree")
if not item.do_action(0):
    sys.exit("FAIL: DoAction on the foot row returned false")
PY
  windows() { "$ANDROMEDA" windows --json | jq "[.[] | select(.app_id == \"foot\")] | length"; }
  wait_for "DoAction on the foot row opened no foot window" "(( \$(windows) >= 1 ))"
  grep "a11y strict" "$OUT/noctalia.log" > "$OUT/unnamed.txt" || true
  echo "unnamed controls: $(wc -l < "$OUT/unnamed.txt") (list in $OUT/unnamed.txt)"
'
echo "PASS; artifacts: $OUT"
