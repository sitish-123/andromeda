#!/usr/bin/env python3
"""Mock org.freedesktop.UDisks2 on DBUS_SYSTEM_BUS_ADDRESS.

dsk.test.UDisks.Plug(name, hint_auto, hint_system) announces a block device with a filesystem through
InterfacesAdded, the way UDisks does on hotplug. Mount, Unmount, and PowerOff calls are appended to argv[1].
"""
import os
import sys

import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

ROOT = "/org/freedesktop/UDisks2"
OM = "org.freedesktop.DBus.ObjectManager"
FS = "org.freedesktop.UDisks2.Filesystem"
DRIVE = "org.freedesktop.UDisks2.Drive"


def record(line):
    with open(sys.argv[1], "a") as out:
        out.write(line + "\n")


class Block(dbus.service.Object):
    def __init__(self, bus, path, name):
        super().__init__(bus, path)
        self.name = name

    @dbus.service.method(FS, in_signature="a{sv}", out_signature="s")
    def Mount(self, options):
        record(f"Mount {self.name}")
        return f"/run/media/test/{self.name}"

    @dbus.service.method(FS, in_signature="a{sv}")
    def Unmount(self, options):
        record(f"Unmount {self.name}")


class Drive(dbus.service.Object):
    @dbus.service.method(DRIVE, in_signature="a{sv}")
    def PowerOff(self, options):
        record("PowerOff")


class Root(dbus.service.Object):
    def __init__(self, bus):
        super().__init__(bus, ROOT)
        self.bus = bus
        self.objects = []

    @dbus.service.method(OM, out_signature="a{oa{sa{sv}}}")
    def GetManagedObjects(self):
        return dbus.Dictionary({}, signature="oa{sa{sv}}")

    @dbus.service.signal(OM, signature="oa{sa{sv}}")
    def InterfacesAdded(self, path, interfaces):
        pass

    @dbus.service.method("dsk.test.UDisks", in_signature="sbb")
    def Plug(self, name, hint_auto, hint_system):
        path = f"{ROOT}/block_devices/{name}"
        drive = f"{ROOT}/drives/{name}"
        self.objects += [Block(self.bus, path, name), Drive(self.bus, drive)]
        block = dbus.Dictionary({
            "HintAuto": dbus.Boolean(hint_auto), "HintIgnore": dbus.Boolean(False),
            "HintSystem": dbus.Boolean(hint_system), "IdLabel": dbus.String(name),
            "IdType": dbus.String("vfat"), "Drive": dbus.ObjectPath(drive),
        }, signature="sv")
        filesystem = dbus.Dictionary({"MountPoints": dbus.Array([], signature="ay")}, signature="sv")
        self.InterfacesAdded(dbus.ObjectPath(path), dbus.Dictionary({
            "org.freedesktop.UDisks2.Block": block, FS: filesystem}, signature="sa{sv}"))


dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.bus.BusConnection(os.environ["DBUS_SYSTEM_BUS_ADDRESS"])
name = dbus.service.BusName("org.freedesktop.UDisks2", bus)
root = Root(bus)
print("ready", flush=True)
GLib.MainLoop().run()
