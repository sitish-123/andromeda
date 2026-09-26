#!/usr/bin/env python3
"""Owns org.freedesktop.locale1 on the bus in DBUS_SYSTEM_BUS_ADDRESS. Appends each call to argv[1] and answers
property reads from an in-memory state that SetLocale/SetX11Keyboard actually update, emitting PropertiesChanged."""
import os
import sys

import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

IFACE = "org.freedesktop.locale1"


class Manager(dbus.service.Object):
    def __init__(self, bus, log):
        super().__init__(bus, "/org/freedesktop/locale1")
        self.log = log
        self.state = {
            "Locale": dbus.Array(["LANG=en_US.UTF-8"], signature="s"),
            "X11Layout": "us",
            "X11Model": "",
            "X11Variant": "",
            "X11Options": "",
            "VConsoleKeymap": "",
        }

    def record(self, line):
        with open(self.log, "a") as out:
            out.write(line + "\n")

    @dbus.service.method(IFACE, in_signature="asb")
    def SetLocale(self, locale, interactive):
        assignments = [str(a) for a in locale]
        self.record("SetLocale " + ",".join(assignments) + f" {bool(interactive)}")
        self.state["Locale"] = dbus.Array(assignments, signature="s")
        self.PropertiesChanged(IFACE, {"Locale": self.state["Locale"]}, [])

    @dbus.service.method(IFACE, in_signature="ssssbb")
    def SetX11Keyboard(self, layout, model, variant, options, convert, interactive):
        self.record(f"SetX11Keyboard {layout} {model} {variant} {options} {bool(convert)} {bool(interactive)}")
        self.state["X11Layout"] = str(layout)
        self.state["X11Model"] = str(model)
        self.state["X11Variant"] = str(variant)
        self.state["X11Options"] = str(options)
        self.PropertiesChanged(
            IFACE,
            {
                "X11Layout": self.state["X11Layout"],
                "X11Model": self.state["X11Model"],
                "X11Variant": self.state["X11Variant"],
                "X11Options": self.state["X11Options"],
            },
            [],
        )

    @dbus.service.method(dbus.PROPERTIES_IFACE, in_signature="ss", out_signature="v")
    def Get(self, interface, name):
        return self.state[name]

    @dbus.service.method(dbus.PROPERTIES_IFACE, in_signature="s", out_signature="a{sv}")
    def GetAll(self, interface):
        return self.state

    @dbus.service.signal(dbus.PROPERTIES_IFACE, signature="sa{sv}as")
    def PropertiesChanged(self, interface, changed, invalidated):
        pass


dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.bus.BusConnection(os.environ["DBUS_SYSTEM_BUS_ADDRESS"])
name = dbus.service.BusName("org.freedesktop.locale1", bus)
manager = Manager(bus, sys.argv[1])
print("ready", flush=True)
GLib.MainLoop().run()
