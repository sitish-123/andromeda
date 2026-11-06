#!/usr/bin/env python3
"""andromeda-update over a private bus with stub sources: checkupdates and flatpak on PATH, a fwupd stub on the bus
(DBUS_SYSTEM_BUS_ADDRESS points at the same bus), and a local news feed. Check reports every group; unread news
blocks the System group until MarkNewsRead. Run: dbus-run-session -- system/tests/update_bus.py [binary]."""
import http.server
import os
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

from gi.repository import Gio, GLib

ROOT = Path(__file__).resolve().parent.parent
BIN = sys.argv[1] if len(sys.argv) > 1 else str(ROOT / "target/debug/andromeda-update")
NAME, PATH = "org.andromeda.Update1", "/org/andromeda/Update1"
FEED = """<?xml version="1.0"?><rss version="2.0"><channel>
<item><title>Manual intervention</title><link>https://archlinux.org/news/x/</link>
<pubDate>Sun, 01 Nov 2026 10:00:00 +0000</pubDate></item></channel></rss>"""
FWUPD_XML = """<node><interface name="org.freedesktop.fwupd">
<method name="GetDevices"><arg type="aa{sv}" direction="out"/></method>
<method name="GetUpgrades"><arg type="s" direction="in"/><arg type="aa{sv}" direction="out"/></method>
</interface></node>"""


def fail(message, log):
    print(f"FAIL: {message}\n{log.read_text()}")
    sys.exit(1)


def stub_fwupd(bus):
    node = Gio.DBusNodeInfo.new_for_xml(FWUPD_XML)

    def on_call(_conn, _sender, _path, _iface, method, params, invocation):
        if method == "GetDevices":
            dev = {"DeviceId": GLib.Variant("s", "dev1"), "Name": GLib.Variant("s", "UEFI dbx"),
                   "Version": GLib.Variant("s", "1")}
            invocation.return_value(GLib.Variant("(aa{sv})", ([dev],)))
        else:
            rel = {"Version": GLib.Variant("s", "2"), "Size": GLib.Variant("t", 4096)}
            invocation.return_value(GLib.Variant("(aa{sv})", ([rel],)))

    bus.register_object("/", node.interfaces[0], on_call, None, None)
    Gio.bus_own_name_on_connection(bus, "org.freedesktop.fwupd", Gio.BusNameOwnerFlags.NONE, None, None)


def main():
    tmp = Path(tempfile.mkdtemp())
    stubs = tmp / "bin"
    stubs.mkdir()
    (stubs / "checkupdates").write_text("#!/bin/sh\necho 'linux 6.9.1-1 -> 6.9.2-1'\n")
    (stubs / "pacman").write_text("#!/bin/sh\necho 'linux 140000000'\n")
    (stubs / "flatpak").write_text("#!/bin/sh\nprintf 'org.gimp.GIMP\\t2.10.38\\t120.5 MB\\n'\n")
    for f in stubs.iterdir():
        f.chmod(0o755)

    class Feed(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            self.send_response(200)
            self.end_headers()
            self.wfile.write(FEED.encode())

        def log_message(self, *_):
            pass

    server = http.server.HTTPServer(("127.0.0.1", 0), Feed)
    threading.Thread(target=server.serve_forever, daemon=True).start()

    bus = Gio.bus_get_sync(Gio.BusType.SESSION)
    # NO_AUTO_START on every call: the session bus also knows the installed Update1 activation file, and an
    # auto-started installed daemon (no stub sources) would answer instead of the one under test.
    stub_fwupd(bus)
    env = dict(os.environ, PATH=f"{stubs}:{os.environ['PATH']}", XDG_CACHE_HOME=str(tmp / "cache"),
               XDG_STATE_HOME=str(tmp / "state"), DBUS_SYSTEM_BUS_ADDRESS=os.environ["DBUS_SESSION_BUS_ADDRESS"],
               ANDROMEDA_UPDATE_FEED=f"http://127.0.0.1:{server.server_port}/")
    log = tmp / "daemon.log"
    daemon = subprocess.Popen([BIN], env=env, stderr=log.open("w"))
    ctx = GLib.MainContext.default()

    def call(method, sig=None, args=None, timeout=20000):
        result = {}

        def done(source, res):
            try:
                result["value"] = source.call_finish(res).unpack()
            except GLib.Error as e:
                result["error"] = e.message

        bus.call(NAME, PATH, NAME, method, GLib.Variant(sig, args) if sig else None, None,
                 Gio.DBusCallFlags.NO_AUTO_START, timeout, None, done)
        deadline = time.monotonic() + timeout / 1000
        while not result and time.monotonic() < deadline:
            ctx.iteration(False)
            time.sleep(0.01)
        if "error" in result:
            if "ServiceUnknown" in result["error"] or "NameHasNoOwner" in result["error"]:
                return None
            fail(f"{method}: {result['error']}", log)
        return result.get("value")

    def prop(name):
        result = {}

        def done(source, res):
            try:
                result["v"] = source.call_finish(res).unpack()[0]
            except GLib.Error as e:
                result["error"] = e.message

        bus.call(NAME, PATH, "org.freedesktop.DBus.Properties", "Get", GLib.Variant("(ss)", (NAME, name)), None,
                 Gio.DBusCallFlags.NO_AUTO_START, 5000, None, done)
        deadline = time.monotonic() + 6
        while not result and time.monotonic() < deadline:
            ctx.iteration(False)
            time.sleep(0.01)
        if "v" not in result:
            fail(f"Get {name}: {result.get('error', 'no reply')}", log)
        return result["v"]

    def check():
        call("Check")
        deadline = time.monotonic() + 30
        while prop("Checking"):
            if time.monotonic() > deadline:
                fail("check never finished", log)
            time.sleep(0.05)

    try:
        for _ in range(50):
            if call("Check") is not None:
                break
            time.sleep(0.1)
        deadline = time.monotonic() + 30
        while prop("Checking"):
            if time.monotonic() > deadline:
                fail("check never finished", log)
            time.sleep(0.05)
        rows = {(group, name): (old, new, size) for group, name, old, new, size in prop("Updates")}
        expect = {
            ("system", "linux"): ("6.9.1-1", "6.9.2-1", 140000000),
            ("apps", "org.gimp.GIMP"): ("", "2.10.38", 120500000),
            ("firmware", "UEFI dbx"): ("1", "2", 4096),
        }
        if rows != expect:
            fail(f"updates {rows}", log)
        if prop("Errors"):
            fail(f"errors {prop('Errors')}", log)
        news = prop("News")
        if len(news) != 1 or not prop("NewsBlocksSystem"):
            fail(f"news {news}", log)
        call("MarkNewsRead", "(x)", (news[0][2],))
        if prop("NewsBlocksSystem") or prop("News"):
            fail("news still blocks after MarkNewsRead", log)
        check()
        if prop("NewsBlocksSystem"):
            fail("read token not kept across checks", log)
        print("PASS")
    finally:
        daemon.terminate()
        server.shutdown()


main()
