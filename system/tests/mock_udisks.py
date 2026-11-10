#!/usr/bin/env python3
# udisks2 on a test system bus. Drives: Stick (whole disk sdb, seat0), Internal (sda, HintSystem), Far (sdd, seat1).
# PowerOff appends "PowerOff <path>" to argv[1].
import os
import sys

import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

ROOT = "/org/freedesktop/UDisks2"
UD = "org.freedesktop.UDisks2"
PROPS = "org.freedesktop.DBus.Properties"


def record(line):
    with open(sys.argv[1], "a") as log:
        log.write(line + "\n")


def block(drive, system=False, partition=False):
    ifaces = {f"{UD}.Block": {"Drive": dbus.ObjectPath(f"{ROOT}/drives/{drive}"), "HintSystem": system}}
    if partition:
        ifaces[f"{UD}.Partition"] = {"Number": dbus.UInt32(1)}
    return ifaces


OBJECTS = {
    f"{ROOT}/block_devices/sdb": block("Stick"),
    f"{ROOT}/block_devices/sdb1": block("Stick", partition=True),
    f"{ROOT}/block_devices/sda": block("Internal", system=True),
    f"{ROOT}/block_devices/sda1": block("Internal", system=True, partition=True),
    f"{ROOT}/block_devices/sdd": block("Far"),
    f"{ROOT}/drives/Stick": {f"{UD}.Drive": {"Seat": "seat0"}},
    f"{ROOT}/drives/Internal": {f"{UD}.Drive": {"Seat": "seat0"}},
    f"{ROOT}/drives/Far": {f"{UD}.Drive": {"Seat": "seat1"}},
}


class UDisks(dbus.service.FallbackObject):
    @dbus.service.method("org.freedesktop.DBus.ObjectManager", out_signature="a{oa{sa{sv}}}")
    def GetManagedObjects(self):
        return OBJECTS

    @dbus.service.method(PROPS, in_signature="ss", out_signature="v", rel_path_keyword="path")
    def Get(self, interface, name, path=None):
        return OBJECTS[ROOT + path][interface][name]

    @dbus.service.method(f"{UD}.Drive", in_signature="a{sv}", rel_path_keyword="path")
    def PowerOff(self, options, path=None):
        record(f"PowerOff {ROOT}{path}")


dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.bus.BusConnection(os.environ["DBUS_SYSTEM_BUS_ADDRESS"])
name = dbus.service.BusName(UD, bus)
UDisks(bus, ROOT)
print("ready", flush=True)
GLib.MainLoop().run()
