#!/usr/bin/env python3
"""Rulebook lint (M5). Output: file:line rule message. Only violations missing from the baseline fail.

  design_lint.py [--root DIR] [--update]
"""
import argparse
import collections
import fnmatch
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCAN = ["shell/src/shell", "libdesk-ui/src/ui"]
EXTS = {".cpp", ".h"}
CALL = r"set(?:Radius|Gap|Padding|FontSize|Size|Width|Height|MinWidth|MinHeight|Spacing|Inset|Thickness)\s*\("
FIELD = r"\.(?:radius|gap|padding|fontSize|width|height|thickness)\s*="
CONST = r"\bconstexpr\s+float\s+k\w*(?:Size|Radius|Gap|Padding|Width|Height|Inset|Spacing|Thickness|Font)\w*\s*="
CTX = re.compile(f"{CALL}|{FIELD}|{CONST}")
LIT = re.compile(r"(?<![\w.])(\d+\.\d*|\d+\.?\d*F|\.\d+F?)(?=[^\w.]|$)")
EASING = re.compile(r"\bEasing::(?!Linear\b)\w+")
STR = re.compile(r'"(?:\\.|[^"\\])*"')


def code_of(line):
    line = STR.sub('""', line)
    return line.split("//", 1)[0]


def literals(code):
    for m in LIT.finditer(code):
        v = float(m.group(1).rstrip("Ff") or 0)
        if v in (0.0, 1.0, 0.5):
            continue
        if re.search(r"[*/]\s*$", code[: m.start()]):
            continue  # scale math
        yield m.group(1)


def norm(text):
    return " ".join(text.split())


def load_allow():
    out = []
    for raw in (HERE / "design_lint_allow.txt").read_text().splitlines():
        if raw.strip() and not raw.startswith("#"):
            rule, glob, sub, reason = (p.strip() for p in raw.split("|", 3))
            if not reason:
                sys.exit(f"allow entry without reason: {raw}")
            out.append((rule, glob, sub))
    return out


def scan(root, allow):
    found = []
    for d in SCAN:
        for p in sorted((root / d).rglob("*")):
            if p.suffix not in EXTS:
                continue
            rel = p.relative_to(root).as_posix()
            for n, line in enumerate(p.read_text(errors="replace").splitlines(), 1):
                code = code_of(line)
                hits = []
                if CTX.search(code):
                    hits += [("S1", f"float size literal {v}; use a token or scale") for v in literals(code)][:1]
                if EASING.search(code):
                    hits.append(("D1", "duration easing; use a spring (animateSpring)"))
                for rule, msg in hits:
                    if any(r == rule and fnmatch.fnmatch(rel, g) and s in line for r, g, s in allow):
                        continue
                    found.append((rel, n, rule, msg, f"{rel}|{rule}|{norm(line)}"))
    return found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=str(HERE.parent.parent))
    ap.add_argument("--update", action="store_true")
    a = ap.parse_args()
    root = Path(a.root)
    found = scan(root, load_allow())
    bfile = HERE / "design_lint_baseline.txt"
    if a.update:
        keys = sorted(f[4] for f in found)
        bfile.write_text("".join(k + "\n" for k in keys))
        per = collections.Counter("/".join(k.split("|")[0].split("/")[:-1]) for k in keys)
        print(f"baseline: {len(keys)} entries")
        for d, c in sorted(per.items()):
            print(f"  {c:4d} {d}")
        return 0
    base = collections.Counter(bfile.read_text().splitlines()) if bfile.exists() else collections.Counter()
    left = collections.Counter(base)
    bad = 0
    for rel, n, rule, msg, key in found:
        if left[key] > 0:
            left[key] -= 1
            continue
        bad += 1
        print(f"{rel}:{n} {rule} {msg}")
    stale = sum(left.values())
    if stale:
        print(f"note: {stale} stale baseline entries; run with --update", file=sys.stderr)
    if bad:
        print(f"design-lint: {bad} new violation(s)", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
