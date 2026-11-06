#!/usr/bin/env python3
"""Mock org.andromeda.Update1 and org.andromeda.UpdateHelper1 on one bus (the E2E bus is both session and system).

MOCK_UPDATES=0 starts up to date; MOCK_NEWS=1 adds an unread Arch news item that blocks System. Calls are appended to
argv[1]. Upgrade emits Progress at 0/40/80 % then Finished(ok); MOCK_UPGRADE=fail ends with Finished(false).
MOCK_ANDROMEDA=1 adds Andromeda rows; UpgradeAndromeda ends with the needs-system refusal.
dsk.test.Update.Progress(s stage, i percent, s package) emits one Progress signal for a still screenshot.
"""
import os
import sys

import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

PROPS = "org.freedesktop.DBus.Properties"
UPDATE = "org.andromeda.Update1"
HELPER = "org.andromeda.UpdateHelper1"
NEWS_TIME = 1762300000


def log(text):
    with open(sys.argv[1], "a") as f:
        f.write(text + "\n")


def rows():
    if os.environ.get("MOCK_UPDATES", "1") == "0":
        return dbus.Array([], signature="(sssst)")
    return dbus.Array([
        *([("andromeda", "andromeda", "1:r15206.g6e98f1910-1", "1:r15209.g6451e8bb8-1", dbus.UInt64(9_000_000)),
           ("andromeda", "andromeda-link-git", "1:r15206.g6e98f1910-1", "1:r15209.g6451e8bb8-1",
            dbus.UInt64(21_000_000))] if os.environ.get("MOCK_ANDROMEDA") else []),
        ("system", "linux", "6.18.2.arch1-1", "6.18.3.arch1-1", dbus.UInt64(148_000_000)),
        ("system", "mesa", "1:26.1.1-1", "1:26.1.2-1", dbus.UInt64(31_000_000)),
        ("system", "firefox", "145.0-1", "145.0.1-1", dbus.UInt64(72_000_000)),
        ("apps", "org.gimp.GIMP", "", "3.2.1", dbus.UInt64(32_000_000)),
        ("firmware", "System Firmware", "1.20", "1.21", dbus.UInt64(9_000_000)),
    ], signature="(sssst)")


class Update(dbus.service.Object):
    def __init__(self, bus):
        super().__init__(bus, "/org/andromeda/Update1")
        news = os.environ.get("MOCK_NEWS", "0") == "1"
        self.props = {
            "Updates": rows(),
            "News": dbus.Array(
                [("Manual intervention for glibc 2.43", "https://archlinux.org/news/glibc-243/", dbus.Int64(NEWS_TIME))]
                if news else [], signature="(ssx)"),
            "NewsBlocksSystem": dbus.Boolean(news),
            "LastCheck": dbus.Int64(NEWS_TIME + 3600),
            "Checking": dbus.Boolean(False),
            "Errors": dbus.Array([], signature="s"),
        }

    @dbus.service.method(PROPS, in_signature="ss", out_signature="v")
    def Get(self, interface, name):
        return self.props[name]

    @dbus.service.method(PROPS, in_signature="s", out_signature="a{sv}")
    def GetAll(self, interface):
        log("GetAll")
        return self.props if interface == UPDATE else {}

    @dbus.service.signal(PROPS, signature="sa{sv}as")
    def PropertiesChanged(self, interface, changed, invalidated):
        pass

    @dbus.service.method(UPDATE)
    def Check(self):
        log("Check")

    @dbus.service.method(UPDATE, in_signature="x")
    def MarkNewsRead(self, until):
        log(f"MarkNewsRead {until}")
        self.props["News"] = dbus.Array([], signature="(ssx)")
        self.props["NewsBlocksSystem"] = dbus.Boolean(False)
        self.PropertiesChanged(UPDATE, {"News": self.props["News"], "NewsBlocksSystem": False}, [])


class Helper(dbus.service.Object):
    def __init__(self, bus):
        super().__init__(bus, "/org/andromeda/UpdateHelper1")
        self.tonight = False

    @dbus.service.method(PROPS, in_signature="s", out_signature="a{sv}")
    def GetAll(self, interface):
        return {"Running": dbus.Boolean(False), "Tonight": dbus.Boolean(self.tonight)} if interface == HELPER else {}

    @dbus.service.method(HELPER, in_signature="b")
    def ScheduleTonight(self, enable):
        log(f"ScheduleTonight {bool(enable)}")
        self.tonight = bool(enable)

    @dbus.service.method(HELPER, in_signature="b")
    def Upgrade(self, dry_run):
        log(f"Upgrade {bool(dry_run)}")
        steps = [("install", 0, "linux"), ("install", 40, "mesa"), ("install", 80, "firefox")]
        for i, step in enumerate(steps):
            GLib.timeout_add(200 * (i + 1), lambda s=step: self.Progress(*s) and False)
        ok = os.environ.get("MOCK_UPGRADE", "ok") != "fail"
        GLib.timeout_add(200 * (len(steps) + 1), lambda: self.Finished(
            ok, "updated" if ok else "commit: could not satisfy dependencies",
            ["linux 6.18.3.arch1-1", "mesa 1:26.1.2-1"] if ok else [], []) and False)

    @dbus.service.method(HELPER)
    def UpgradeAndromeda(self):
        log("UpgradeAndromeda")
        self.Progress("install", 50, "andromeda")
        GLib.timeout_add(400, lambda: self.Finished(
            False, "needs a system update first: glibc", [], []) and False)

    @dbus.service.method(HELPER, in_signature="x")
    def MarkNewsRead(self, until):
        log(f"HelperMarkNewsRead {until}")

    @dbus.service.signal(HELPER, signature="sis")
    def Progress(self, stage, percent, package):
        pass

    @dbus.service.signal(HELPER, signature="bsasas")
    def Finished(self, ok, message, targets, questions):
        pass

    @dbus.service.method("dsk.test.Update", in_signature="sis")
    def EmitProgress(self, stage, percent, package):
        self.Progress(stage, percent, package)


def main():
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SessionBus()
    keep = [dbus.service.BusName(UPDATE, bus), dbus.service.BusName(HELPER, bus), Update(bus), Helper(bus)]
    print("ready", flush=True)
    GLib.MainLoop().run()
    del keep


if __name__ == "__main__":
    main()
