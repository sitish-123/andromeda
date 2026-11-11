#!/usr/bin/env python3
"""`andromeda debug crashes --json` and `debug report` against fixture crash files and fake journal tools."""

import json
import os
import pathlib
import subprocess
import sys
import tempfile
import time

andromeda = sys.argv[1]
now = int(time.time())


def iso(t):
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(t))


def basic(t):
    return time.strftime("%Y%m%dT%H%M%SZ", time.gmtime(t))


def fail(message, output=""):
    print(f"FAIL: {message}\n{output}", file=sys.stderr)
    sys.exit(1)


with tempfile.TemporaryDirectory() as root:
    root = pathlib.Path(root)
    crashes = root / "state/andromeda/crashes"
    crashes.mkdir(parents=True)
    (crashes / f"andromeda-{basic(now - 1800)}-100.txt").write_text(
        f"component: andromeda\nversion: 0.9.0 (gabc123)\npid: 100\nsignal: SIGSEGV (11)\nfault_address: 0x0\n"
        f"time: {iso(now - 1800)}\nbacktrace:\n"
        "#0 0x7f0000001000 /usr/lib/libc.so.6+0x1000\n"
        "#1 0x55d000002000 /nonexistent/andromeda+0x2a2b\n"
        "#2 0x55d000003000 /nonexistent/andromeda+0x3000\n"
        "log:\n10:00:00.000 E [render] boom\n"
    )
    (crashes / f"andromeda-shell-{basic(now - 600)}-200.txt").write_text(
        f"component: andromeda-shell\nversion: v0.9.0\npid: 200\nsignal: SIGABRT (6)\ntime: {iso(now - 600)}\n"
        "backtrace:\n#0 0x1 /nonexistent/andromeda-shell.so+0x10\nlog:\n"
    )
    (crashes / f"andromeda-system-session-{basic(now - 300)}-400.txt").write_text(
        f"component: andromeda-system-session\nversion: 0.1.0\npid: 400\nsignal: panic\ntime: {iso(now - 300)}\n"
        "message: index out of bounds\nlocation: src/battery.rs:10:5\nbacktrace:\n   0: main\nlog:\n"
    )
    (crashes / f"andromeda-{basic(now - 90000)}-50.txt").write_text(
        f"component: andromeda\nversion: old\npid: 50\nsignal: SIGSEGV (11)\ntime: {iso(now - 90000)}\nbacktrace:\nlog:\n"
    )

    bin_dir = root / "bin"
    bin_dir.mkdir()
    coredumps = [
        {"time": (now - 600) * 1_000_000, "pid": 200, "sig": 6, "exe": "/usr/bin/andromeda-shell"},
        {"time": (now - 120) * 1_000_000, "pid": 300, "sig": 11, "exe": "/usr/bin/andromeda-linkd"},
        {"time": (now - 100) * 1_000_000, "pid": 301, "sig": 11, "exe": "/usr/bin/firefox"},
    ]
    journal = [
        {"__REALTIME_TIMESTAMP": str((now - 1200) * 1_000_000), "SYSLOG_IDENTIFIER": "andromeda", "PRIORITY": "6",
         "MESSAGE": "10:40:00.000 [\u001b[31mERR\u001b[0m] [\u001b[34mdrm\u001b[0m] page flip failed"},
        {"__REALTIME_TIMESTAMP": str((now - 1100) * 1_000_000), "SYSLOG_IDENTIFIER": "andromeda-shell",
         "PRIORITY": "6", "MESSAGE": "10:41:00.000 [\u001b[33mWRN\u001b[0m] tray slow"},
        {"__REALTIME_TIMESTAMP": str((now - 1000) * 1_000_000), "SYSLOG_IDENTIFIER": "andromeda-shell",
         "PRIORITY": "6", "MESSAGE": "10:42:00.000 [\u001b[32mINF\u001b[0m] ready"},
        {"__REALTIME_TIMESTAMP": str((now - 900) * 1_000_000), "SYSLOG_IDENTIFIER": "andromeda-system",
         "PRIORITY": "4", "MESSAGE": "[WARN andromeda_system::battery] cell imbalance"},
    ]
    (root / "coredumps.json").write_text(json.dumps(coredumps))
    (root / "journal.json").write_text("".join(json.dumps(entry) + "\n" for entry in journal))
    fakes = {
        "coredumpctl": f"cat '{root}/coredumps.json'",
        "journalctl": f"echo \"$@\" >> '{root}/journalctl.args'; cat '{root}/journal.json'",
        "systemctl": 'case "$1" in --user) printf "andromeda.service loaded active running A\\n'
        'andromeda-shell.service loaded failed failed S\\n";; esac',
        "pacman": "exit 1",
        "dpkg": "exit 1",
        "andromeda-shell": "echo 'noctalia v0.9.0'",
    }
    for name, body in fakes.items():
        path = bin_dir / name
        path.write_text(f"#!/bin/sh\n{body}\n")
        path.chmod(0o755)

    env = dict(os.environ, XDG_STATE_HOME=str(root / "state"), XDG_CONFIG_HOME=str(root / "config"),
               HOME=str(root), PATH=f"{bin_dir}:{os.environ.get('PATH', '/usr/bin')}")

    def run(*args):
        return subprocess.run([andromeda, "debug", *args], env=env, capture_output=True, text=True, timeout=60)

    out = run("crashes", "--json", "--since", "1d")
    if out.returncode != 0:
        fail(f"debug crashes exit {out.returncode}", out.stderr)
    entries = json.loads(out.stdout)
    by_pid = {e["pid"]: e for e in entries if e["pid"] in (100, 200, 300, 400, 50, 301)}
    if sorted(by_pid) != [100, 200, 300, 400]:
        fail(f"pids {sorted(by_pid)}", out.stdout)
    if [e["pid"] for e in entries].count(200) != 1 or not by_pid[200]["coredump"]:
        fail("shell crash file and core dump not merged", out.stdout)
    compositor = by_pid[100]
    if (compositor["signal"], compositor["version"], compositor["top_frame"]) != (
            "SIGSEGV", "0.9.0 (gabc123)", "andromeda+0x2a2a"):
        fail("compositor crash fields", out.stdout)
    if by_pid[300]["component"] != "andromeda-linkd" or by_pid[300]["signal"] != "SIGSEGV":
        fail("core-dump-only crash", out.stdout)
    if not by_pid[400]["top_frame"].startswith("src/battery.rs:10:5: index out of bounds"):
        fail("panic location", out.stdout)

    if run("crashes", "--since", "soon").returncode != 2:
        fail("bad duration is not a usage error")

    out = run("report", "--since", "1h")
    lines = out.stdout.splitlines()
    if out.returncode != 1:
        fail(f"report exit {out.returncode}", out.stdout + out.stderr)
    if len(lines) > 40:
        fail(f"report has {len(lines)} lines", out.stdout)
    if not (lines[0].startswith("first failure: ") and "andromeda SIGSEGV at andromeda+0x2a2a" in lines[0]):
        fail("first line does not name the compositor crash", out.stdout)
    for expected in ("version  andromeda-shell  noctalia v0.9.0", "units: 1 running, 1 failed: andromeda-shell.service",
                     "andromeda error [drm] page flip failed", "crashes: 4"):
        if not any(expected in line for line in lines):
            fail(f"report lacks '{expected}'", out.stdout)
    report_dir = pathlib.Path(lines[-1].removeprefix("full report: "))
    for name in ("summary.txt", "journal.txt", "crashes.json", "units.txt", "config.txt", "versions.txt"):
        if not (report_dir / name).is_file():
            fail(f"report dir lacks {name}", out.stdout)
    if "tray slow" not in (report_dir / "journal.txt").read_text():
        fail("journal.txt lacks the warning")

    out = run("log", "shell", "--since", "1h")
    if out.returncode != 0 or "andromeda-shell warn tray slow" not in out.stdout or "ready" in out.stdout:
        fail("debug log shell", out.stdout + out.stderr)
    if " -p warning " not in (root / "journalctl.args").read_text().splitlines()[-1] + " ":
        fail("debug log does not filter with journalctl -p warning", (root / "journalctl.args").read_text())
    if "andromeda-system warn cell imbalance" not in out.stdout:
        fail("priority-4 Rust line not shown at warn without its prefix", out.stdout)

print("ok")
