#!/usr/bin/env python3
"""Usage: tools/check-file.py <path>. Checks one file after an edit and prints one ok line or the first error.

C/C++: a -fsyntax-only compile with the flags from the file's build dir (compile_commands.json); a header borrows the
flags of a source file in its directory. Rust: cargo check -p <crate>. Python: py_compile. Shell: bash -n.
"""
import glob
import json
import os
import re
import shlex
import subprocess
import sys
import tomllib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# The first build dir per top directory; CLAUDE.md names one dir per purpose.
PREFERRED = {
    "compositor": ["compositor/build-debug"],
    "shell": ["shell/build-f7"],
    "libdesk-ui": ["shell/build-f7", "greeter/build-release"],
    "libdesk-system": ["shell/build-f7"],
    "protocol": ["compositor/build-debug", "shell/build-f7"],
    "greeter": ["greeter/build-release"],
    "portal": ["portal/build-debug"],
}
C_EXT = {".c", ".cc", ".cpp", ".cxx"}
H_EXT = {".h", ".hh", ".hpp", ".hxx"}
ERROR = re.compile(r":\d+(:\d+)?: (fatal )?error")


def rel(path, base):
    p = os.path.realpath(os.path.join(base, path))
    return os.path.relpath(p, ROOT) if p.startswith(ROOT + "/") else p


def first_error(text, base):
    lines = [l for l in text.splitlines() if l.strip()]
    for line in lines:
        if ERROR.search(line) or line.startswith("error"):
            m = re.match(r"([^:\s]+)(:\d+.*)", line)
            return rel(m.group(1), base) + m.group(2) if m else line
    return lines[0] if lines else "failed with no output"


def compile_dbs(top):
    preferred = [os.path.join(ROOT, d) for d in PREFERRED.get(top, [])]
    others = sorted(glob.glob(f"{ROOT}/*/build*/") + glob.glob(f"{ROOT}/apps/*/build*/"))
    seen = []
    for d in preferred + [o.rstrip("/") for o in others]:
        if d not in seen and os.path.isfile(f"{d}/compile_commands.json"):
            seen.append(d)
    return seen


def find_entry(path):
    top = os.path.relpath(path, ROOT).split("/")[0]
    header = os.path.splitext(path)[1] in H_EXT
    for d in compile_dbs(top):
        with open(f"{d}/compile_commands.json") as f:
            entries = json.load(f)
        for e in entries:
            src = os.path.realpath(os.path.join(e["directory"], e["file"]))
            if (os.path.dirname(src) == os.path.dirname(path)) if header else (src == path):
                return d, e
    return None, None


def check_c(path):
    d, e = find_entry(path)
    if not e:
        return 2, "no build dir compiles this file or its directory (compile_commands.json)"
    args = e["arguments"] if "arguments" in e else shlex.split(e["command"])
    out, skip = [], False
    for a in args:
        if skip:
            skip = False
        elif a in ("-o", "-MF", "-MQ", "-MT"):
            skip = True
        elif a in ("-c", "-MD", "-MMD") or a.startswith("-fdiagnostics-color"):
            pass
        elif a == e["file"]:
            if os.path.splitext(path)[1] in H_EXT:
                out += ["-Wno-pragma-once-outside-header", "-x", "c-header" if e["file"].endswith(".c") else "c++-header"]
            out.append(path)
        else:
            out.append(a)
    out += ["-fsyntax-only", "-fdiagnostics-color=never"]
    # A -fsyntax-only compile writes no object or depfile, so it is safe in the shared shell/build-f7 without ninja.
    r = subprocess.run(out, cwd=e["directory"], capture_output=True, text=True)
    label = f"{os.path.basename(args[0]) if args[0] != 'ccache' else args[1]} -fsyntax-only in {os.path.relpath(d, ROOT)}"
    return (0, label) if r.returncode == 0 else (1, first_error(r.stderr + r.stdout, e["directory"]))


def check_rust(path):
    crate = ws = None
    d = os.path.dirname(path)
    while d.startswith(ROOT):
        manifest = os.path.join(d, "Cargo.toml")
        if os.path.isfile(manifest):
            with open(manifest, "rb") as f:
                data = tomllib.load(f)
            if crate is None and "package" in data:
                crate = data["package"]["name"]
                ws = d
            if "workspace" in data:
                ws = d
                break
        d = os.path.dirname(d)
    if not crate:
        return 2, "no Cargo.toml with a [package] above this file"
    r = subprocess.run(
        ["cargo", "check", "-p", crate, "--all-targets", "--message-format", "short", "--color", "never", "-q"],
        cwd=ws, capture_output=True, text=True,
    )
    label = f"cargo check -p {crate}"
    return (0, label) if r.returncode == 0 else (1, first_error(r.stderr, ws))


def check_python(path, shown):
    try:
        with open(path, "rb") as f:
            compile(f.read(), path, "exec")
    except SyntaxError as e:
        return 1, f"{shown}:{e.lineno}: {type(e).__name__}: {e.msg}"
    return 0, "python compile"


def check_tool(cmd, label):
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    return (0, label) if r.returncode == 0 else (1, first_error(r.stderr + r.stdout, ROOT))


def main():
    if len(sys.argv) != 2:
        print("usage: tools/check-file.py <path>", file=sys.stderr)
        return 2
    path = os.path.realpath(sys.argv[1])
    shown = os.path.relpath(path, ROOT)
    if not os.path.isfile(path):
        print(f"check-file {shown}: no such file")
        return 2
    ext = os.path.splitext(path)[1]
    with open(path, "rb") as f:
        head = f.readline()
    if ext in C_EXT | H_EXT:
        rc, msg = check_c(path)
    elif ext == ".rs":
        rc, msg = check_rust(path)
    elif ext == ".py":
        rc, msg = check_python(path, shown)
    elif ext in (".sh", ".bash") or re.match(rb"#!.*\bbash\b", head):
        rc, msg = check_tool(["bash", "-n", shown], "bash -n")
    else:
        rc, msg = 2, f"no check for '{ext or 'no extension'}' files"
    print(f"check-file {shown}: {'ok (' + msg + ')' if rc == 0 else ('FAILED ' if rc == 1 else '') + msg}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
