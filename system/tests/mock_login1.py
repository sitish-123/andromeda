#!/usr/bin/env python3
# logind on a test system bus: session "c1" of the caller's uid (no session by PID, so GetUser's Display finds it),
# session "c2" of uid 4242, and the inhibitors that argv[2] holds at each call, one "what mode uid" per line. Power,
# firmware-setup and brightness calls only append a line to argv[1]; nothing reboots or suspends.
import os
import sys

import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

ROOT = "/org/freedesktop/login1"
MANAGER = "org.freedesktop.login1.Manager"
SESSION = "org.freedesktop.login1.Session"
PROPS = "org.freedesktop.DBus.Properties"
UID = os.getuid()
SESSIONS = {"c1": UID, "c2": 4242}
LOG = sys.argv[1]
INHIBITORS = sys.argv[2]
KEEP = []


def record(line):
    with open(LOG, "a") as log:
        log.write(line + "\n")


class Login1(dbus.service.FallbackObject):
    def __init__(self, bus):
        super().__init__(bus, ROOT)

    @dbus.service.method(MANAGER, out_signature="a(susso)")
    def ListSessions(self):
        return [(sid, dbus.UInt32(uid), f"user{uid}", "seat0", dbus.ObjectPath(f"{ROOT}/session/{sid}"))
                for sid, uid in SESSIONS.items()]

    @dbus.service.method(MANAGER, out_signature="a(ssssuu)")
    def ListInhibitors(self):
        with open(INHIBITORS) as rows:
            return [(what, "app", "why", mode, dbus.UInt32(int(uid)), dbus.UInt32(1))
                    for what, mode, uid in (row.split() for row in rows if row.strip())]

    @dbus.service.method(MANAGER, in_signature="u", out_signature="o")
    def GetSessionByPID(self, pid):
        raise dbus.exceptions.DBusException("no session", name="org.freedesktop.login1.NoSessionForPID")

    @dbus.service.method(MANAGER, in_signature="s", out_signature="o")
    def GetSession(self, sid):
        return dbus.ObjectPath(f"{ROOT}/session/{sid}")

    @dbus.service.method(MANAGER, in_signature="u", out_signature="o")
    def GetUser(self, uid):
        return dbus.ObjectPath(f"{ROOT}/user/_{uid}")

    @dbus.service.method(MANAGER, out_signature="s")
    def CanReboot(self):
        return "yes"

    @dbus.service.method(MANAGER, out_signature="s")
    def CanSuspend(self):
        return "yes"

    @dbus.service.method(MANAGER, out_signature="s")
    def CanRebootToFirmwareSetup(self):
        return "na"

    @dbus.service.method(MANAGER, in_signature="t")
    def RebootWithFlags(self, flags):
        record(f"RebootWithFlags {flags}")

    @dbus.service.method(MANAGER, in_signature="t")
    def SuspendWithFlags(self, flags):
        record(f"SuspendWithFlags {flags}")

    @dbus.service.method(MANAGER, in_signature="b")
    def SetRebootToFirmwareSetup(self, enable):
        record(f"SetRebootToFirmwareSetup {bool(enable)}")

    @dbus.service.method(MANAGER, in_signature="ssss", out_signature="h")
    def Inhibit(self, what, who, why, mode):
        read, write = os.pipe()
        KEEP.append(write)
        record(f"Inhibit {what} {mode}")
        return dbus.types.UnixFd(read)

    @dbus.service.method(SESSION, in_signature="ssu", rel_path_keyword="path")
    def SetBrightness(self, subsystem, name, value, path=None):
        record(f"SetBrightness {path} {subsystem} {name} {value}")

    @dbus.service.method(PROPS, in_signature="ss", out_signature="v", rel_path_keyword="path")
    def Get(self, interface, name, path=None):
        if path.startswith("/user/"):
            return dbus.Struct(("c1", dbus.ObjectPath(f"{ROOT}/session/c1")))
        sid = path.rsplit("/", 1)[-1]
        if name == "User":
            uid = SESSIONS[sid]
            return dbus.Struct((dbus.UInt32(uid), dbus.ObjectPath(f"{ROOT}/user/_{uid}")))
        return "user"


dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.bus.BusConnection(os.environ["DBUS_SYSTEM_BUS_ADDRESS"])
name = dbus.service.BusName("org.freedesktop.login1", bus)
Login1(bus)
print("ready", flush=True)
GLib.MainLoop().run()
