#!/usr/bin/env bash
# Usage: tools/where.sh <word>. Prints features from docs/map.toml whose name, keywords or notes match.
set -uo pipefail
[ $# -ge 1 ] || { echo "usage: tools/where.sh <word>" >&2; exit 2; }
root=$(cd "$(dirname "$0")/.." && pwd)
python3 -I - "$root/docs/map.toml" "$*" <<'PY'
import sys
import tomllib

path, query = sys.argv[1], sys.argv[2].lower()
with open(path, "rb") as f:
    features = tomllib.load(f).get("feature", [])
hits = []
for feat in features:
    text = " ".join([feat.get("name", ""), " ".join(feat.get("keywords", [])), feat.get("notes", "")])
    if query in text.lower():
        hits.append(feat)
for feat in hits[:15]:
    entry = ", ".join(feat.get("entry", [])) or "-"
    test = feat.get("test") or "-"
    print(f"{feat['name']} | {feat['module']} | {entry} | {test} | {feat['build']}")
if not hits:
    print(f"no feature matches '{query}'")
elif len(hits) > 15:
    print(f"... {len(hits) - 15} more; narrow the word")
PY
