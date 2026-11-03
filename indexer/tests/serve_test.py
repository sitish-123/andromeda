#!/usr/bin/env python3
# Starts `andromeda-index serve` on a temp home with no index: it must crawl, answer on its socket, and find a file
# written after start within 1 s.
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

exe = sys.argv[1]
base = tempfile.mkdtemp(prefix="serve-test-")
home = os.path.join(base, "home")
env = dict(os.environ, HOME=home, XDG_STATE_HOME=os.path.join(base, "state"),
           XDG_RUNTIME_DIR=os.path.join(base, "run"))
os.makedirs(os.path.join(home, "Documents"))
os.makedirs(env["XDG_RUNTIME_DIR"])
open(os.path.join(home, "Documents", "quarterly-budget.ods"), "w").close()
sock_path = os.path.join(env["XDG_RUNTIME_DIR"], "andromeda-index.sock")
failures = []


def check(ok, what):
    if not ok:
        failures.append(what)
        print("FAIL:", what, file=sys.stderr)


def ask(conn, line):
    conn.sendall(line.encode() + b"\n")
    data = b""
    while not data.endswith(b"\n\n") and data != b"\n":
        chunk = conn.recv(65536)
        if not chunk:
            break
        data += chunk
    return [r.split("\t") for r in data.decode().split("\n") if r]


daemon = subprocess.Popen([exe, "serve"], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    deadline = time.monotonic() + 10
    while not os.path.exists(sock_path) and time.monotonic() < deadline:
        time.sleep(0.02)
    conn = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    conn.settimeout(5)
    conn.connect(sock_path)

    rows = ask(conn, "query 20 budget")
    check([r[0] for r in rows] == [os.path.join(home, "Documents", "quarterly-budget.ods")], "crawled file is found")
    check(len(rows) == 1 and rows[0][3] == "f" and rows[0][2] == "0", "row has size and kind")
    check(ask(conn, "nonsense") == [], "unknown request gets an empty answer")

    open(os.path.join(home, "Documents", "fresh-invoice.pdf"), "w").close()
    written = time.monotonic()
    found = False
    while not found and time.monotonic() - written < 1.0:
        found = any(r[0].endswith("fresh-invoice.pdf") for r in ask(conn, "query 20 invoice"))
        time.sleep(0.02)
    check(found, "a new file is found within 1 s")
    conn.close()
finally:
    daemon.terminate()
    daemon.wait()
    shutil.rmtree(base)

print("PASS" if not failures else f"{len(failures)} failures")
sys.exit(1 if failures else 0)
