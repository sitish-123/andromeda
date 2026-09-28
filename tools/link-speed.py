#!/usr/bin/env python3
"""Times a desktop-to-phone Link transfer on the running umbriel-linkd and appends the result to
artifacts/link-speed/results.jsonl: size, seconds, MiB/s, and the path the session was on (from the daemon's
"connected over" journal line).

Usage: tools/link-speed.py [MIB] [LABEL]   e.g. tools/link-speed.py 100 wifi
"""
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import time

from gi.repository import Gio, GLib

NAME, PATH, IFACE = "org.umbriel.Link1", "/org/umbriel/Link1", "org.umbriel.Link1"
OUT = pathlib.Path(__file__).resolve().parent.parent / "artifacts" / "link-speed"


def connected_device(bus):
    reply = bus.call_sync(NAME, PATH, "org.freedesktop.DBus.Properties", "Get", GLib.Variant("(ss)", (IFACE, "Devices")),
                          GLib.VariantType("(v)"), Gio.DBusCallFlags.NONE, -1, None)
    devices = [d for d in reply.unpack()[0] if d[2]]
    if not devices:
        sys.exit("no connected device")
    return devices[0][0], devices[0][1]


def session_path(device_id):
    log = subprocess.run(["journalctl", "--user", "-u", "umbriel-linkd", "-b", "--no-pager", "-o", "cat"],
                         capture_output=True, text=True).stdout
    lines = [line for line in log.splitlines() if f"{device_id}: connected over" in line]
    return lines[-1].split("connected over ", 1)[1] if lines else "unknown"


def main() -> int:
    mib = int(sys.argv[1]) if len(sys.argv) > 1 else 100
    label = sys.argv[2] if len(sys.argv) > 2 else ""
    bus = Gio.bus_get_sync(Gio.BusType.SESSION)
    device_id, device_name = connected_device(bus)
    loop = GLib.MainLoop()
    state = {"id": None, "status": None, "early": {}}

    def on_signal(_conn, _sender, _path, _iface, member, params):
        if member != "TransferFinished":
            return
        transfer, status = params.unpack()[:2]
        if state["id"] is None:
            state["early"][transfer] = status
        elif transfer == state["id"]:
            state["status"] = status
            loop.quit()

    bus.signal_subscribe(NAME, IFACE, "TransferFinished", PATH, None, Gio.DBusSignalFlags.NONE, on_signal)
    with tempfile.NamedTemporaryFile(prefix="link-speed-", suffix=".bin") as blob:
        for _ in range(mib):
            blob.write(os.urandom(1 << 20))
        blob.flush()
        path = session_path(device_id)
        fds = Gio.UnixFDList()
        files = [(fds.append(os.open(blob.name, os.O_RDONLY)), f"link-speed-{mib}MiB.bin")]
        started = time.monotonic()
        reply, _ = bus.call_with_unix_fd_list_sync(NAME, PATH, IFACE, "SendFiles",
                                                   GLib.Variant("(sa(hs))", (device_id, files)),
                                                   GLib.VariantType("(s)"), Gio.DBusCallFlags.NONE, -1, fds, None)
        state["id"] = reply.unpack()[0]
        state["status"] = state["early"].get(state["id"])
        if state["status"] is None:
            GLib.timeout_add_seconds(1800, loop.quit)
            loop.run()
        seconds = time.monotonic() - started
    result = {
        "t": time.strftime("%Y-%m-%dT%H:%M:%S"), "label": label, "device": device_name, "path": path,
        "path_after": session_path(device_id), "bytes": mib << 20, "status": state["status"],
        "seconds": round(seconds, 2), "mib_per_s": round(mib / seconds, 2),
    }
    OUT.mkdir(parents=True, exist_ok=True)
    with open(OUT / "results.jsonl", "a") as out:
        out.write(json.dumps(result) + "\n")
    print(json.dumps(result))
    return 0 if state["status"] == "done" else 1


if __name__ == "__main__":
    sys.exit(main())
