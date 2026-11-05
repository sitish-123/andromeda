#!/usr/bin/env python3
# Stands in for at-spi-bus-launcher on a private session bus: owns org.a11y.Bus, returns the address of an
# accessibility bus started by the caller (argv[1]) and reports AT-SPI as enabled.
import sys

from gi.repository import Gio, GLib

ADDRESS = sys.argv[1]
XML = """
<node>
  <interface name="org.a11y.Bus"><method name="GetAddress"><arg type="s" direction="out"/></method></interface>
  <interface name="org.a11y.Status">
    <property name="IsEnabled" type="b" access="readwrite"/>
    <property name="ScreenReaderEnabled" type="b" access="readwrite"/>
  </interface>
</node>
"""


def call(_conn, _sender, _path, _iface, method, _params, invocation):
    if method == "GetAddress":
        invocation.return_value(GLib.Variant("(s)", (ADDRESS,)))


def get_property(*_args):
    return GLib.Variant("b", True)


def on_bus(conn, _name):
    for iface in Gio.DBusNodeInfo.new_for_xml(XML).interfaces:
        conn.register_object("/org/a11y/bus", iface, call, get_property, None)


Gio.bus_own_name(Gio.BusType.SESSION, "org.a11y.Bus", Gio.BusNameOwnerFlags.NONE, on_bus, None, None)
GLib.MainLoop().run()
