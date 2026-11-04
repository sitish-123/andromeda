#!/usr/bin/env python3
"""Owns org.freedesktop.login1 on the bus in DBUS_SYSTEM_BUS_ADDRESS and appends each power call and inhibitor to argv[1]."""
import os
import sys

import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

MANAGER = "org.freedesktop.login1.Manager"


class Delayed:
    """Answers every method call after MOCK_DELAY seconds (default 0)."""

    def _message_cb(self, connection, message):
        delay = float(os.environ.get("MOCK_DELAY", "0"))
        if delay <= 0:
            return super()._message_cb(connection, message)
        GLib.timeout_add(int(delay * 1000), lambda: super(Delayed, self)._message_cb(connection, message) and False)


class Manager(Delayed, dbus.service.Object):
    def __init__(self, bus, log):
        super().__init__(bus, "/org/freedesktop/login1")
        self.log = log
        # "yes" unless the test overrides it, to gate Hibernate/SuspendThenHibernate like real hardware without swap.
        self.can_hibernate = os.environ.get("MOCK_CAN_HIBERNATE", "yes")
        self.can_suspend_then_hibernate = os.environ.get("MOCK_CAN_SUSPEND_THEN_HIBERNATE", "yes")

    def record(self, method, interactive):
        with open(self.log, "a") as out:
            out.write(f"{method} {bool(interactive)}\n")

    @dbus.service.method(MANAGER, in_signature="b")
    def Suspend(self, interactive):
        self.record("Suspend", interactive)

    @dbus.service.method(MANAGER, in_signature="b")
    def Reboot(self, interactive):
        self.record("Reboot", interactive)

    @dbus.service.method(MANAGER, in_signature="b")
    def PowerOff(self, interactive):
        self.record("PowerOff", interactive)

    @dbus.service.method(MANAGER, in_signature="b")
    def Hibernate(self, interactive):
        self.record("Hibernate", interactive)

    @dbus.service.method(MANAGER, in_signature="b")
    def SuspendThenHibernate(self, interactive):
        self.record("SuspendThenHibernate", interactive)

    @dbus.service.method(MANAGER, in_signature="ssss", out_signature="h")
    def Inhibit(self, what, who, why, mode):
        self.record_line(f"Inhibit {what} {who} {mode}")
        read_end, write_end = os.pipe()
        GLib.io_add_watch(read_end, GLib.IO_HUP, lambda fd, _: self.release(fd, what, who))
        fd = dbus.types.UnixFd(write_end)
        os.close(write_end)
        return fd

    def release(self, fd, what, who):
        self.record_line(f"Release {what} {who}")
        os.close(fd)
        return False

    def record_line(self, line):
        with open(self.log, "a") as out:
            out.write(line + "\n")

    @dbus.service.method(MANAGER, in_signature="t")
    def SuspendWithFlags(self, flags):
        self.record_line(f"SuspendWithFlags {flags}")

    @dbus.service.method(MANAGER, in_signature="t")
    def RebootWithFlags(self, flags):
        self.record_line(f"RebootWithFlags {flags}")

    @dbus.service.method(MANAGER, in_signature="t")
    def PowerOffWithFlags(self, flags):
        self.record_line(f"PowerOffWithFlags {flags}")

    @dbus.service.method(MANAGER, out_signature="a(ssssuu)")
    def ListInhibitors(self):
        # MOCK_BLOCK="sleep:shutdown" lists one block inhibitor held by another app.
        what = os.environ.get("MOCK_BLOCK", "")
        return [(what, "Mock App", "test", "block", dbus.UInt32(1000), dbus.UInt32(1))] if what else []

    @dbus.service.method(MANAGER, out_signature="s")
    def CanHibernate(self):
        return self.can_hibernate

    @dbus.service.method(MANAGER, out_signature="s")
    def CanSuspendThenHibernate(self):
        return self.can_suspend_then_hibernate

    def properties(self):
        # MOCK_LID_BATTERY etc. override the logind defaults (logind.conf(5)).
        return {
            "HandleLidSwitch": os.environ.get("MOCK_LID_BATTERY", "suspend"),
            # logind returns "" when HandleLidSwitchExternalPower= is unset.
            "HandleLidSwitchExternalPower": os.environ.get("MOCK_LID_POWER", ""),
            "HandleLidSwitchDocked": os.environ.get("MOCK_LID_DOCKED", "ignore"),
            "HandlePowerKey": os.environ.get("MOCK_POWER_KEY", "poweroff"),
        }

    @dbus.service.method(dbus.PROPERTIES_IFACE, in_signature="ss", out_signature="v")
    def Get(self, interface, prop):
        return self.properties()[prop]

    @dbus.service.method(dbus.PROPERTIES_IFACE, in_signature="s", out_signature="a{sv}")
    def GetAll(self, interface):
        return self.properties()


dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.bus.BusConnection(os.environ["DBUS_SYSTEM_BUS_ADDRESS"])
name = dbus.service.BusName("org.freedesktop.login1", bus)
manager = Manager(bus, sys.argv[1])
print("ready", flush=True)
GLib.MainLoop().run()
