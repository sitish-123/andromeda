#!/usr/bin/env python3
"""`andromeda update` against fake org.andromeda.Update1 and org.andromeda.UpdateHelper1 on one private bus (the CLI
reaches the system bus through DBUS_SYSTEM_BUS_ADDRESS). Run: dbus-run-session -- cli_update_bus.py <andromeda>."""
import os
import subprocess
import sys
import tempfile
import threading
from pathlib import Path

from gi.repository import Gio, GLib

ANDROMEDA = sys.argv[1]
UPDATE_XML = """<node><interface name="org.andromeda.Update1">
<method name="Check"/>
<property name="Checking" type="b" access="read"/>
<property name="Updates" type="a(sssst)" access="read"/>
<property name="Errors" type="as" access="read"/>
</interface></node>"""
HELPER_XML = """<node><interface name="org.andromeda.UpdateHelper1">
<method name="Upgrade"><arg type="b" direction="in"/></method>
<signal name="Progress"><arg type="s"/><arg type="i"/><arg type="s"/></signal>
<signal name="Finished"><arg type="b"/><arg type="s"/><arg type="as"/><arg type="as"/><arg type="b"/></signal>
</interface></node>"""
ROWS = [("system", "linux", "6.9.1-1", "6.9.2-1", 140000000)]
HELPER = "org.andromeda.UpdateHelper1"
APPS = [("apps", "org.gimp.GIMP", "2.10.38", "3.0.0", 0)]

state = {"mode": "ok", "checking": False, "updates": [], "upgrades": []}


def main():
    bus = Gio.bus_get_sync(Gio.BusType.SESSION)
    other = Gio.DBusConnection.new_for_address_sync(
        os.environ["DBUS_SESSION_BUS_ADDRESS"],
        Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)

    def name_call(conn, method, args):
        conn.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", method,
                       GLib.Variant("(su)" if len(args) == 2 else "(s)", args), None, Gio.DBusCallFlags.NONE, -1, None)

    def properties_changed(changed):
        bus.emit_signal(None, "/org/andromeda/Update1", "org.freedesktop.DBus.Properties", "PropertiesChanged",
                        GLib.Variant("(sa{sv}as)", ("org.andromeda.Update1", changed, [])))

    def check_done():
        state["updates"] = [] if state["mode"] in ("none", "errors") else \
            ROWS + (APPS if state["mode"].startswith("apps") else [])
        state["checking"] = False
        properties_changed({"Checking": GLib.Variant("b", False)})
        return False

    def on_update(_conn, _sender, _path, _iface, method, _params, invocation):
        state["checking"] = True
        properties_changed({"Checking": GLib.Variant("b", True)})
        invocation.return_value(None)
        GLib.timeout_add(100, check_done)

    def on_update_property(_conn, _sender, _path, _iface, name):
        return {"Checking": GLib.Variant("b", state["checking"]),
                "Updates": GLib.Variant("a(sssst)", state["updates"]),
                "Errors": GLib.Variant("as", ["firmware: fwupd is not running"] if state["mode"] == "errors" else [])}[name]

    def helper_signal(name, value):
        bus.emit_signal(None, "/org/andromeda/UpdateHelper1", "org.andromeda.UpdateHelper1", name, value)

    def on_helper(_conn, _sender, _path, _iface, method, params, invocation):
        state["upgrades"].append(params.unpack())
        if state["mode"] == "denied":
            invocation.return_dbus_error("org.freedesktop.DBus.Error.AccessDenied",
                                         "not authorized: org.andromeda.update.upgrade")
            return
        if state["mode"] == "stale":
            # The name changes hands and goes unowned before the reply: a NameOwnerChanged whose old owner is not
            # the connection that answers, as an earlier helper's idle exit sends.
            name_call(other, "RequestName", (HELPER, 0))
            name_call(bus, "ReleaseName", (HELPER,))
            name_call(other, "ReleaseName", (HELPER,))
            name_call(bus, "RequestName", (HELPER, 4))
        invocation.return_value(None)
        helper_signal("Progress", GLib.Variant("(sis)", ("download", 0, "linux")))
        helper_signal("Progress", GLib.Variant("(sis)", ("install", 40, "linux")))
        if state["mode"] == "gone":
            name_call(bus, "ReleaseName", (HELPER,))
            name_call(bus, "RequestName", (HELPER, 4))
        elif state["mode"] == "fail":
            helper_signal("Finished", GLib.Variant("(bsasasb)", (False, "conflicting files: linux", [], [], False)))
        else:
            helper_signal("Finished", GLib.Variant("(bsasasb)", (True, "updated", ["linux 6.9.2-1"], [], False)))

    bus.register_object("/org/andromeda/Update1", Gio.DBusNodeInfo.new_for_xml(UPDATE_XML).interfaces[0],
                        on_update, on_update_property, None)
    bus.register_object("/org/andromeda/UpdateHelper1", Gio.DBusNodeInfo.new_for_xml(HELPER_XML).interfaces[0],
                        on_helper, None, None)
    for name in ("org.andromeda.Update1", "org.andromeda.UpdateHelper1"):
        bus.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
                      GLib.Variant("(su)", (name, 4)), None, Gio.DBusCallFlags.NONE, -1, None)
    threading.Thread(target=GLib.MainLoop().run, daemon=True).start()

    tmp = Path(tempfile.mkdtemp())
    flatpak_log = tmp / "flatpak.log"
    (tmp / "flatpak").write_text(
        '#!/bin/sh\necho "$@" >> "$FLATPAK_LOG"\n'
        'if [ "$FLATPAK_FAIL" = 1 ]; then echo "error: remote flathub not found" >&2; exit 1; fi\n'
        'printf "Updating org.gimp.GIMP\\r100%%\\n"\n')
    (tmp / "flatpak").chmod(0o755)
    env = dict(os.environ, DBUS_SYSTEM_BUS_ADDRESS=os.environ["DBUS_SESSION_BUS_ADDRESS"],
               PATH=f"{tmp}:{os.environ['PATH']}", FLATPAK_LOG=str(flatpak_log))
    failures = []

    def case(mode, want_code, want_out, want_upgrades, want_flatpak=""):
        state["mode"], state["upgrades"] = mode, []
        flatpak_log.write_text("")
        env["FLATPAK_FAIL"] = "1" if mode == "apps-fail" else "0"
        run = subprocess.run([ANDROMEDA, "update"], env=env, stdin=subprocess.DEVNULL, capture_output=True, text=True,
                             timeout=60)
        output = run.stdout + run.stderr
        flatpak = flatpak_log.read_text().strip()
        ok = (run.returncode == 0) == (want_code == 0) and all(w in output for w in want_out) \
            and (mode == "none" or "up to date" not in output) \
            and state["upgrades"] == want_upgrades and flatpak == want_flatpak
        print(f"{'PASS' if ok else 'FAIL'} {mode}: exit {run.returncode}, upgrades {state['upgrades']}, "
              f"flatpak {flatpak!r}\n{output}")
        if not ok:
            failures.append(mode)

    case("ok", 0, ["linux 6.9.1-1 -> 6.9.2-1", "install linux 40%", "updated"], [(False,)])
    case("fail", 1, ["conflicting files: linux"], [(False,)])
    case("denied", 1, ["authorization refused: not authorized"], [(False,)])
    case("none", 0, ["System is up to date"], [])
    flatpak_args = "update --noninteractive -y"
    case("apps", 0, ["org.gimp.GIMP 2.10.38 -> 3.0.0 (apps)", "updated", "apps 100%"], [(False,)], flatpak_args)
    case("apps-fail", 1, ["flatpak failed: error: remote flathub not found"], [(False,)], flatpak_args)
    case("gone", 1, ["the update service stopped"], [(False,)])
    case("stale", 0, ["updated"], [(False,)])
    case("errors", 1, ["check failed: firmware: fwupd is not running"], [])
    sys.exit(1 if failures else 0)


main()
