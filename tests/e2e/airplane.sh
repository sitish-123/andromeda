#!/usr/bin/env bash
# Airplane mode writes one rfkill event that soft-blocks every radio: `noctalia msg airplane-toggle` with
# NOCTALIA_RFKILL_DEVICE pointing at a file (never the machine's /dev/rfkill) must write a struct rfkill_event with
# type RFKILL_TYPE_ALL (0) and op RFKILL_OP_CHANGE_ALL (3), soft set to the opposite of the current state read from
# /sys/class/rfkill. Writes event.txt to $OUT (default ./artifacts/airplane).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/airplane}
source "$(dirname "$0")/lib.sh"
boot_headless 1
: > "$OUT/rfkill.bin"

export NOCTALIA_RFKILL_DEVICE="$OUT/rfkill.bin"
with_noctalia '
  before=$("$NOCTALIA" msg airplane-status)
  "$NOCTALIA" msg airplane-toggle > /dev/null
  for _ in $(seq 30); do [[ -s $NOCTALIA_RFKILL_DEVICE ]] && break; sleep 0.1; done
  echo "before=$before" > "$OUT/event.txt"
'
read -r _ type op soft _ < <(od -An -tu1 -j3 -N5 "$OUT/rfkill.bin" | awk '{print "x", $2, $3, $4, $5}')
before=$(sed -n 's/before=//p' "$OUT/event.txt")
want_soft=$([[ $before == off ]] && echo 1 || echo 0)
echo "type=$type op=$op soft=$soft (airplane was $before)" | tee -a "$OUT/event.txt"
[[ $type == 0 && $op == 3 && $soft == "$want_soft" ]] || { echo "FAIL: unexpected rfkill event"; exit 1; }
echo "PASS; artifacts: $OUT"
