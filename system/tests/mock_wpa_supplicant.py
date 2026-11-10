#!/usr/bin/env python3
# wpa_supplicant on a test system bus: CreateInterface, GetInterface and RemoveInterface edit the fake nl80211 file
# (argv[1], ANDROMEDA_SYSTEM_FAKE_WIFI: one `name ifindex wiphy iftype up freq` line each) and log each call to
# argv[2]. A created interface goes on the card of the first line, type AP (3), down.
import os
import sys

import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

WPA = "fi.w1.wpa_supplicant1"
WPA_PATH = "/fi/w1/wpa_supplicant1"
WIFI, LOG = sys.argv[1], sys.argv[2]


def rows():
    with open(WIFI) as f:
        return [line.split() for line in f if line.strip()]


def write(table):
    with open(WIFI, "w") as f:
        f.writelines(" ".join(row) + "\n" for row in table)


def log(line):
    with open(LOG, "a") as f:
        f.write(line + "\n")


class Supplicant(dbus.service.Object):
    def __init__(self, bus):
        super().__init__(bus, WPA_PATH)

    @staticmethod
    def path(ifindex):
        return dbus.ObjectPath(f"{WPA_PATH}/Interfaces/{ifindex}")

    @dbus.service.method(WPA, in_signature="a{sv}", out_signature="o")
    def CreateInterface(self, args):
        name, kind = str(args["Ifname"]), str(args.get("Type", ""))
        log(f"CreateInterface {name} {kind} {bool(args.get('Create', False))}")
        table = rows()
        if any(row[0] == name for row in table):
            raise dbus.DBusException(f"{name} exists", name=f"{WPA}.InterfaceExists")
        ifindex = max((int(row[1]) for row in table), default=0) + 1
        table.append([name, str(ifindex), table[0][2], "3" if kind == "ap" else "2", "0", "0"])
        write(table)
        return self.path(ifindex)

    @dbus.service.method(WPA, in_signature="s", out_signature="o")
    def GetInterface(self, name):
        log(f"GetInterface {name}")
        for row in rows():
            if row[0] == name:
                return self.path(row[1])
        raise dbus.DBusException(f"no {name}", name=f"{WPA}.InterfaceUnknown")

    @dbus.service.method(WPA, in_signature="o")
    def RemoveInterface(self, path):
        log(f"RemoveInterface {path}")
        table = rows()
        kept = [row for row in table if self.path(row[1]) != path]
        if len(kept) == len(table):
            raise dbus.DBusException(f"no {path}", name=f"{WPA}.InterfaceUnknown")
        write(kept)


dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.bus.BusConnection(os.environ["DBUS_SYSTEM_BUS_ADDRESS"])
name = dbus.service.BusName(WPA, bus)
supplicant = Supplicant(bus)
print("ready", flush=True)
GLib.MainLoop().run()
