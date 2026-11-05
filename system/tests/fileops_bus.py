#!/usr/bin/env python3
"""andromeda-fileops over a private session bus: a copy conflict answered with ResolveConflict, Undo, Trash and
Restore. Run: dbus-run-session -- system/tests/fileops_bus.py [andromeda-fileops binary]."""
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from gi.repository import Gio, GLib

ROOT = Path(__file__).resolve().parent.parent
BIN = sys.argv[1] if len(sys.argv) > 1 else str(ROOT / "target/debug/andromeda-fileops")
NAME, MANAGER = "org.andromeda.FileOps1", "/org/andromeda/FileOps1"


def fail(message, daemon_log):
    print(f"FAIL: {message}\n{daemon_log.read_text()}")
    sys.exit(1)


def main():
    tmp = Path(tempfile.mkdtemp())
    os.environ["XDG_DATA_HOME"] = str(tmp / "data")
    (tmp / "data").mkdir()
    (tmp / "src").mkdir()
    (tmp / "dst").mkdir()
    (tmp / "src/a").write_text("new")
    (tmp / "dst/a").write_text("old")
    log = tmp / "daemon.log"
    daemon = subprocess.Popen([BIN], stderr=log.open("w"))
    bus = Gio.bus_get_sync(Gio.BusType.SESSION)

    def call(path, iface, method, sig, args):
        deadline = time.monotonic() + 5
        while True:
            try:
                reply = bus.call_sync(NAME, path, iface, method, GLib.Variant(sig, args) if sig else None, None,
                                      Gio.DBusCallFlags.NONE, 5000, None)
                return reply.unpack()
            except GLib.Error as e:
                if "ServiceUnknown" not in e.message or time.monotonic() > deadline:
                    fail(f"{method}: {e.message}", log)
                time.sleep(0.1)

    def start(method, sig, args):
        return call(MANAGER, NAME, method, sig, args)[0]

    def until_state(job, want):
        for _ in range(50):
            state = call(job, "org.freedesktop.DBus.Properties", "Get", "(ss)", (f"{NAME}.Job", "State"))[0]
            if state == want:
                return
            time.sleep(0.1)
        fail(f"{job} stayed {state}, wanted {want}", log)

    def b(path):
        return str(path).encode()

    try:
        job = start("Copy", "(aayay)", ([b(tmp / "src/a")], b(tmp / "dst")))
        until_state(job, "conflict")
        call(job, f"{NAME}.Job", "ResolveConflict", "(sb)", ("replace", False))
        until_state(job, "done")
        if (tmp / "dst/a").read_text() != "new":
            fail("replace did not copy", log)

        undo = start("Undo", "(o)", (job,))
        until_state(undo, "done")
        if (tmp / "dst/a").read_text() != "old":
            fail("undo did not bring the replaced file back", log)

        trash = start("Trash", "(aay)", ([b(tmp / "dst/a")],))
        until_state(trash, "done")
        if (tmp / "dst/a").exists():
            fail("trash left the file", log)

        restore = start("Restore", "(a(ays))", ([(b(tmp / "data/Trash"), "a")],))
        until_state(restore, "done")
        if (tmp / "dst/a").read_text() != "old":
            fail("restore did not put the file back", log)
        print("PASS")
    finally:
        daemon.terminate()


main()
