#!/usr/bin/env python3
"""Launch-time measurements for docs/performance.md, run by tools/measure-launch.sh inside a headless session.

First frame = the first output commit after the start mark; an idle headless output commits nothing.
Shell IPC goes straight to the socket, so no client process start counts. Compositor polls spawn its CLI
(a few ms per poll; the poll cost is printed with the results).
"""
import ctypes
import json
import os
import signal
import socket
import statistics
import subprocess
import sys
import time

ANDROMEDA = os.environ["ANDROMEDA"]
NOCTALIA = os.environ["NOCTALIA"]
OUT = os.environ["OUT"]
RUNS = int(os.environ.get("RUNS", "5"))
APPS = os.environ.get("APPS", "foot gnome-calculator dolphin").split()
MODES = os.environ.get("MODES", "Clipboard Emoji Gifs Stickers Symbols").split()
SOCK = os.path.join(os.environ["XDG_RUNTIME_DIR"], "noctalia-" + os.environ["WAYLAND_DISPLAY"] + ".sock")
TIMEOUT_S = 20.0


def commits():
    out = subprocess.run([ANDROMEDA, "output-commits", "--json"], capture_output=True, text=True).stdout
    try:
        data = json.loads(out)
        data = data.get("ok", data)
        return int(data.get("HEADLESS-1", 0))
    except ValueError:
        return -1


def window_count():
    out = subprocess.run([ANDROMEDA, "windows"], capture_output=True, text=True).stdout
    return len([line for line in out.splitlines() if line.strip()])


def ms_since(start):
    return (time.monotonic() - start) * 1000.0


def wait_for(predicate, what):
    deadline = time.monotonic() + TIMEOUT_S
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.002)
    raise SystemExit("timeout waiting for " + what)


def wait_idle():
    """Until the output has not committed for 300 ms."""
    last, quiet_since = commits(), time.monotonic()
    deadline = time.monotonic() + TIMEOUT_S
    while time.monotonic() < deadline:
        time.sleep(0.05)
        now = commits()
        if now != last:
            last, quiet_since = now, time.monotonic()
        elif time.monotonic() - quiet_since >= 0.3:
            return
    raise SystemExit("output never went idle")


def ipc(command):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.connect(SOCK)
        s.sendall(("/\x1e" + command).encode())
        s.shutdown(socket.SHUT_WR)
        return s.makefile().read()


PR_SET_CHILD_SUBREAPER = 36


def descendants():
    children = {}
    for entry in os.listdir("/proc"):
        if entry.isdigit():
            try:
                with open(f"/proc/{entry}/stat") as f:
                    ppid = int(f.read().rsplit(")", 1)[1].split()[1])
            except (OSError, IndexError, ValueError):
                continue
            children.setdefault(ppid, []).append(int(entry))
    found, todo = [], [os.getpid()]
    while todo:
        for child in children.get(todo.pop(), []):
            found.append(child)
            todo.append(child)
    return found


def stop_descendants(sig, keep):
    """Apps such as `code` detach into a new session; as a child subreaper this script still owns them."""
    for pid in descendants():
        if pid not in keep:
            try:
                os.kill(pid, sig)
            except ProcessLookupError:
                pass


def msg(*args):
    """Untimed shell IPC through the CLI client (the raw socket got a reset on panel-close, cause unknown)."""
    return subprocess.run([NOCTALIA, "msg", *args], capture_output=True, text=True).returncode == 0


def first_commit_after(action):
    wait_idle()
    base = commits()
    start = time.monotonic()
    action()
    wait_for(lambda: commits() > base, "a commit")
    return ms_since(start)


def shell_open_ms(command, log_path):
    """Open to first presented frame, from the shell's own log line (no poll delay)."""
    wait_idle()
    with open(log_path) as f:
        f.seek(0, os.SEEK_END)
        ipc(command)
        deadline = time.monotonic() + TIMEOUT_S
        while time.monotonic() < deadline:
            line = f.readline()
            if "open to first frame" in line:
                return float(line.rsplit("frame", 1)[1].split()[0])
            if not line:
                time.sleep(0.002)
    raise SystemExit("timeout waiting for the open log line of " + command)


def summary(name, values):
    line = f"{name}: median {statistics.median(values):.0f} ms (runs {', '.join(f'{v:.0f}' for v in values)})"
    print(line, flush=True)
    return line


def main():
    # Orphans of a detaching app re-parent to this script, so stop_descendants() reaches them.
    ctypes.CDLL(None, use_errno=True).prctl(PR_SET_CHILD_SUBREAPER, 1)
    lines = []
    poll = []
    for _ in range(20):
        start = time.monotonic()
        commits()
        poll.append(ms_since(start))
    lines.append(f"poll cost: median {statistics.median(poll):.1f} ms per compositor query")
    print(lines[-1], flush=True)

    # Shell start. Run 0 starts with empty caches (first login); runs 1..RUNS reuse them.
    shell = None
    starts = []
    for i in range(RUNS + 1):
        log = open(os.path.join(OUT, f"shell-{i}.log"), "w")
        holder = {}
        starts.append(first_commit_after(lambda: holder.update(p=subprocess.Popen([NOCTALIA], stdout=log, stderr=log))))
        shell = holder["p"]
        wait_for(lambda: msg("panel-state"), "shell IPC")
        wait_idle()
        with open(os.path.join(OUT, f"shell-{i}.log")) as f:
            breakdown = [l.split("startup", 1)[-1].strip() for l in f if "first frame at" in l]
        print(f"shell run {i}: {starts[-1]:.0f} ms; {breakdown[0] if breakdown else 'no breakdown line'}", flush=True)
        if i < RUNS:
            shell.terminate()
            shell.wait(timeout=TIMEOUT_S)
    lines.append(f"shell start, empty caches: {starts[0]:.0f} ms")
    print(lines[-1], flush=True)
    lines.append(summary("shell start to first frame", starts[1:]))

    shell_log = os.path.join(OUT, f"shell-{RUNS}.log")
    opens = []
    for _ in range(RUNS):
        opens.append(shell_open_ms("panel-open launcher", shell_log))
        wait_idle()
        msg("panel-close")
    lines.append(summary("launcher open to first frame", opens))

    for mode in MODES:
        times = []
        for _ in range(RUNS):
            times.append(shell_open_ms("panel-open launcher mode:" + mode, shell_log))
            wait_idle()
            msg("panel-close")
        lines.append(summary(f"launcher {mode} mode open to first frame", times))

    for app in APPS:
        times = []
        for _ in range(RUNS):
            wait_idle()
            base = window_count()
            keep = set(descendants())
            start = time.monotonic()
            proc = subprocess.Popen([app], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
            wait_for(lambda: window_count() > base, app + " window")
            times.append(ms_since(start))
            stop_descendants(signal.SIGTERM, keep)
            try:
                proc.wait(timeout=TIMEOUT_S)
            except subprocess.TimeoutExpired:
                stop_descendants(signal.SIGKILL, keep)
            wait_for(lambda: window_count() <= base, app + " close")
        lines.append(summary(f"{app} exec to first window", times))

    shell.terminate()
    with open(os.path.join(OUT, "results.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    sys.exit(main())
