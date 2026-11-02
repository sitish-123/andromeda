#!/usr/bin/env bash
# ANDROMEDA_DEV_TOKENS reloads the design tokens live: after the TOML copy changes, the bar looks different within 2 s
# (island inset and the window color), an invalid file keeps the last good look, and deleting the keys reverts it. Writes before.png, after.png,
# invalid.png, deleted.png, and result.txt to $OUT (default ./artifacts/dev-tokens).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/dev-tokens}
source "$(dirname "$0")/lib.sh"
mkdir -p "$OUT"
export ANDROMEDA_DEV_TOKENS=$OUT/design-tokens.toml
cp "$ROOT/protocol/design-tokens.toml" "$ANDROMEDA_DEV_TOKENS"
export ANDROMEDA_DEV_TOKENS
boot_headless 1

with_noctalia '
  sleep 2 # real time: the bar settles
  grim "$OUT/before.png"
  sed -i -e "s/^inset_text = 12.0/inset_text = 40.0/" -e "s/^window = \"#[0-9a-f]*\"/window = \"#ff0000ff\"/" \
    -e "s/^view = \"#[0-9a-f]*\"/view = \"#ff0000ff\"/" -e "s/^header = \"#[0-9a-f]*\"/header = \"#ff0000ff\"/" \
    -e "s/^solid_regular = \"#[0-9a-f]*\"/solid_regular = \"#ff0000ff\"/" -e "s/^tint_regular = \"#[0-9a-f]*\"/tint_regular = \"#ff0000ff\"/" \
    "$ANDROMEDA_DEV_TOKENS"
  sleep 1.5 # real time: the reload
  grim "$OUT/after.png"
  echo "no_such_key = 1" >> "$ANDROMEDA_DEV_TOKENS"
  sleep 1.5
  grim "$OUT/invalid.png"
  sed -i -e "/^no_such_key/d" -e "/^inset_text = 40.0/d" -e "/= \"#ff0000ff\"/d" "$ANDROMEDA_DEV_TOKENS"
  sleep 1.5
  grim "$OUT/deleted.png"
'
grep -E "dev-tokens" "$OUT/noctalia.log" | tee "$OUT/result.txt" || true
python3 - "$OUT" <<'PY' | tee -a "$OUT/result.txt"
import sys
from PIL import Image, ImageChops
out = sys.argv[1]
img = {n: Image.open(f"{out}/{n}.png").convert("RGB") for n in ("before", "after", "invalid", "deleted")}
changed = ImageChops.difference(img["before"], img["after"]).getbbox() is not None
kept = ImageChops.difference(img["after"], img["invalid"]).getbbox() is None
left = (0, 0, 200, 60)  # the left island has no clock
reverted = ImageChops.difference(img["before"].crop(left), img["deleted"].crop(left)).getbbox() is None
print(f"reload_changed={changed} invalid_keeps_last_good={kept} deleted_keys_revert={reverted}")
sys.exit(0 if changed and kept and reverted else 1)
PY
