#!/usr/bin/env python3
"""Fail when a module depends on a module that docs/modules.toml does not allow."""
import os
import re
import subprocess
import sys
import tomllib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MODULES = tomllib.load(open(os.path.join(ROOT, "docs/modules.toml"), "rb"))["modules"]
REL_PATH = re.compile(r"""['"](\.\./[^'"]*)['"]""")


def module_of(path):
    rel = os.path.relpath(os.path.normpath(path), ROOT)
    return None if rel.startswith("..") else rel.split(os.sep)[0]


def edges():
    files = subprocess.run(["git", "-C", ROOT, "ls-files", "-s"], capture_output=True, text=True, check=True)
    for line in files.stdout.splitlines():
        meta, path = line.split("\t", 1)
        if path.startswith("link/") and path != "link":
            continue
        src = os.path.join(ROOT, path)
        if meta.startswith("120000"):
            yield path, os.path.join(os.path.dirname(src), os.readlink(src))
        elif os.path.basename(path) in ("meson.build", "Cargo.toml"):
            for target in REL_PATH.findall(open(src, encoding="utf-8").read()):
                yield path, os.path.join(os.path.dirname(src), target)


def main():
    errors = set()
    for path, target in edges():
        src, dst = module_of(os.path.join(ROOT, path)), module_of(target)
        if src not in MODULES:
            errors.add(f"{path}: {src}/ is not a module in docs/modules.toml")
        elif dst and dst != src and not MODULES[src].get("meta") and dst not in MODULES[src]["deps"]:
            errors.add(f"{path}: {src} -> {dst} is not allowed by docs/modules.toml")
    for e in sorted(errors):
        print(e)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
