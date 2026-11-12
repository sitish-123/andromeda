#!/usr/bin/env bash
# Fails when a path in docs/map.toml does not exist, a build value is not a module, or a scenario is not a scenario file.
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
    if "scenario" in feat:
        scn = feat["scenario"]
        if not (isinstance(scn, str) and scn.startswith("tests/scenario/") and scn.endswith(".toml")):
            bad.append(f"{name}: scenario '{scn}' is not a tests/scenario/*.toml path")
        elif not os.path.isfile(os.path.join(root, scn)):
            bad.append(f"{name}: missing scenario '{scn}'")
for line in bad:
    print("FAIL " + line)
print(f"map_test: {len(features)} features, {len(bad)} problems")
sys.exit(1 if bad else 0)
PY
