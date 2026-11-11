#!/usr/bin/env bash
# Fails when a path in docs/map.toml does not exist, or a build value is not a module.
set -uo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
map=$root/docs/map.toml

python3 -I - "$root" "$map" <<'PY'
import os
import sys
import tomllib

root, path = sys.argv[1], sys.argv[2]
modules = {"compositor", "shell", "greeter", "system", "link"}
with open(path, "rb") as f:
    features = tomllib.load(f).get("feature", [])
bad = []
for feat in features:
    name = feat.get("name", "<no name>")
    for key in ("name", "module", "entry", "test", "build"):
        if key not in feat:
            bad.append(f"{name}: missing field '{key}'")
    if feat.get("build") not in modules:
        bad.append(f"{name}: build '{feat.get('build')}' is not one of {sorted(modules)}")
    for entry in feat.get("entry", []):
        if not os.path.exists(os.path.join(root, entry)):
            bad.append(f"{name}: missing path '{entry}'")
for line in bad:
    print("FAIL " + line)
print(f"map_test: {len(features)} features, {len(bad)} problems")
sys.exit(1 if bad else 0)
PY
