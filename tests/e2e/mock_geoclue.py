#!/usr/bin/env python3
"""A stand-in for GeoClue2 on the test bus: one client whose Start reports a fixed location. It appends every call and
property set to the log given as the first argument, so a test can check what the shell asked for."""
import sys
import warnings

from gi.repository import Gio, GLib

warnings.filterwarnings("ignore", category=DeprecationWarning)

LOG = open(sys.argv[1], "a", buffering=1)
LATITUDE, LONGITUDE, DESCRIPTION = 27.7172, 85.3240, "Kathmandu"
CLIENT = "/org/freedesktop/GeoClue2/Client/1"
LOCATION = "/org/freedesktop/GeoClue2/Location/1"
XML = """<node>
  <interface name="org.freedesktop.GeoClue2.Manager">
    <method name="GetClient"><arg name="client" type="o" direction="out"/></method>
  </interface>
  <interface name="org.freedesktop.GeoClue2.Client">
    <method name="Start"/>
    <method name="Stop"/>
    <property name="DesktopId" type="s" access="readwrite"/>
    <property name="RequestedAccuracyLevel" type="u" access="readwrite"/>
    <property name="Location" type="o" access="read"/>
    <signal name="LocationUpdated"><arg name="old" type="o"/><arg name="new" type="o"/></signal>
  </interface>
  <interface name="org.freedesktop.GeoClue2.Location">
    <property name="Latitude" type="d" access="read"/>
    <property name="Longitude" type="d" access="read"/>
    <property name="Accuracy" type="d" access="read"/>
    <property name="Description" type="s" access="read"/>
  </interface>
</node>"""
node = Gio.DBusNodeInfo.new_for_xml(XML)
client_props = {"DesktopId": GLib.Variant("s", ""), "RequestedAccuracyLevel": GLib.Variant("u", 0),
                "Location": GLib.Variant("o", "/")}
location_props = {"Latitude": GLib.Variant("d", LATITUDE), "Longitude": GLib.Variant("d", LONGITUDE),
                  "Accuracy": GLib.Variant("d", 5000.0), "Description": GLib.Variant("s", DESCRIPTION)}


def call(conn, sender, path, iface, method, params, invocation):
    LOG.write(f"call {iface}.{method}\n")
    if method == "GetClient":
        invocation.return_value(GLib.Variant("(o)", (CLIENT,)))
    elif method == "Start":
        invocation.return_value(None)
        client_props["Location"] = GLib.Variant("o", LOCATION)
        conn.emit_signal(None, CLIENT, "org.freedesktop.GeoClue2.Client", "LocationUpdated",
                         GLib.Variant("(oo)", ("/", LOCATION)))
    else:
        invocation.return_value(None)


def get(props):
    return lambda conn, sender, path, iface, name: props[name]


def set_client(conn, sender, path, iface, name, value):
    LOG.write(f"set {name}={value.unpack()}\n")
    client_props[name] = value
    return True


def on_bus(conn, name):
    conn.register_object("/org/freedesktop/GeoClue2/Manager", node.interfaces[0], call, None, None)
    conn.register_object(CLIENT, node.interfaces[1], call, get(client_props), set_client)
    conn.register_object(LOCATION, node.interfaces[2], call, get(location_props), None)


def on_name(conn, name):
    LOG.write("ready\n")


Gio.bus_own_name(Gio.BusType.SYSTEM, "org.freedesktop.GeoClue2", Gio.BusNameOwnerFlags.NONE, on_bus, on_name, None)
GLib.MainLoop().run()
