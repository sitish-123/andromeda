#!/usr/bin/env bash
# Settings navigation after the regrouping (gaps 3.1-3.3, 3.7). Fails when:
#   - a setting disappears or a new one appears without settings_paths.txt being updated (the registry's config
#     paths must equal that list exactly),
#   - a config path is shown on two pages,
#   - an entry lands in a section the sidebar never shows,
#   - `settings-open <page>` (or `bar:<name>`) does not switch pages while Settings is already open (two pages
#     paint identically),
#   - an old section id (templates, osd, umbriel) no longer opens the page that absorbed it.
# Writes the registry listing and one screenshot per page to $OUT (default ./artifacts/settings-navigation).
set -euo pipefail
OUT=${OUT:-$(pwd)/artifacts/settings-navigation}
source "$(dirname "$0")/lib.sh"
boot_headless 1

run "$NOCTALIA" config settings-count --list > "$OUT/registry.txt"
[[ -s $OUT/registry.txt ]] || { echo "FAIL: empty registry listing" >&2; exit 1; }

# Entries without a config path are action buttons (reset usage, browse store); they carry no setting.
awk -F '\t' '$3 != "" { print $3 }' "$OUT/registry.txt" | sort > "$OUT/paths.txt"
if ! diff -u "$(dirname "$0")/settings_paths.txt" "$OUT/paths.txt" > "$OUT/paths.diff"; then
  echo "FAIL: settings paths changed (see $OUT/paths.diff)" >&2
  exit 1
fi
dupes=$(uniq -d "$OUT/paths.txt")
[[ -z $dupes ]] || { echo "FAIL: paths on more than one page: $dupes" >&2; exit 1; }

pages=(appearance wallpaper text-scale motion dock panels launcher control-center notifications desktop overview
  windows keybinds services power security location calendar screenshot system shell hooks)
for section in $(cut -f1 "$OUT/registry.txt" | sort -u); do
  [[ $section == bar ]] && continue
  printf '%s\n' "${pages[@]}" | grep -qx "$section" || { echo "FAIL: entries in unlisted section $section" >&2; exit 1; }
done

# One setting changed from Settings, so the Style page shows a row's reset control.
mkdir -p "$RUNTIME/home/.local/state/noctalia"
printf '[theme]\npure_black_dark = true\n' > "$RUNTIME/home/.local/state/noctalia/settings.toml"

aliases="templates:appearance osd:notifications umbriel:overview"
with_noctalia '
  shot() { "$NOCTALIA" msg settings-open "$1" > /dev/null; sleep 1.5; grim -g "0,40 1280x680" "$OUT/$2.png"; }  # real time: repaint; below the bar, whose clock ticks
  for page in '"${pages[*]}"'; do shot "$page" "$page"; done
  for pair in '"$aliases"'; do shot "${pair%%:*}" "alias-${pair%%:*}"; done
  shot bar:default bar
'

for page in "${pages[@]}" bar; do
  [[ -s $OUT/$page.png ]] || { echo "FAIL: no screenshot for $page" >&2; exit 1; }
done
repeated=$(for page in "${pages[@]}" bar; do md5sum < "$OUT/$page.png"; done | sort | uniq -d | head -1)
[[ -z $repeated ]] || { echo "FAIL: two pages painted identically; settings-open did not navigate" >&2; exit 1; }
for pair in $aliases; do
  cmp -s "$OUT/alias-${pair%%:*}.png" "$OUT/${pair##*:}.png" \
    || { echo "FAIL: settings-open ${pair%%:*} did not open ${pair##*:}" >&2; exit 1; }
done
echo "PASS: $(wc -l < "$OUT/paths.txt") settings on ${#pages[@]} pages, none lost or duplicated; artifacts: $OUT"
