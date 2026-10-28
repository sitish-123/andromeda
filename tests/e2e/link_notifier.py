#!/usr/bin/env python3
"""A minimal org.freedesktop.Notifications server for sessions without one. Logs each call as a JSON line to LOG and
reads commands from stdin: `invoke ID KEY` emits ActionInvoked, `dismiss ID` emits NotificationClosed with reason 2.
Usage: link_notifier.py LOG."""
import json
import sys

from gi.repository import Gio, GLib

XML = """
<node><interface name="org.freedesktop.Notifications">
  <method name="GetCapabilities"><arg type="as" direction="out"/></method>
  <method name="Notify">
    <arg type="s" direction="in"/><arg type="u" direction="in"/><arg type="s" direction="in"/>
    <arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="as" direction="in"/>
    <arg type="a{sv}" direction="in"/><arg type="i" direction="in"/><arg type="u" direction="out"/>
  </method>
  <method name="CloseNotification"><arg type="u" direction="in"/></method>
  <method name="GetServerInformation">
    <arg type="s" direction="out"/><arg type="s" direction="out"/><arg type="s" direction="out"/>
    <arg type="s" direction="out"/>
  </method>
  <signal name="ActionInvoked"><arg type="u"/><arg type="s"/></signal>
  <signal name="NotificationClosed"><arg type="u"/><arg type="u"/></signal>
</interface></node>
"""
PATH = "/org/freedesktop/Notifications"
INTERFACE = "org.freedesktop.Notifications"

log = open(sys.argv[1], "a", buffering=1)
connection = None
next_id = 1


def record(event, **fields):
    log.write(json.dumps({"event": event, **fields}) + "\n")


def emit(name, args):
    connection.emit_signal(None, PATH, INTERFACE, name, GLib.Variant("(us)" if name == "ActionInvoked" else "(uu)", args))


def call(_connection, _sender, _path, _interface, method, params, invocation):
    global next_id
    if method == "GetCapabilities":
        invocation.return_value(GLib.Variant("(as)", (["actions", "body"],)))
    elif method == "Notify":
        app, replaces, icon, summary, body, actions, hints, _ = params.unpack()
        shown = replaces or next_id
        next_id += replaces == 0
        record("notify", id=shown, app=app, icon=icon, summary=summary, body=body, actions=actions,
               hints=sorted(hints))
        invocation.return_value(GLib.Variant("(u)", (shown,)))
    elif method == "CloseNotification":
        (shown,) = params.unpack()
        record("close", id=shown)
        emit("NotificationClosed", (shown, 3))
        invocation.return_value(None)
    else:
        invocation.return_value(GLib.Variant("(ssss)", ("link-notifier", "andromeda", "1", "1.2")))


def command(source, _condition):
    line = source.readline()
    if not line:
        return False
    words = line.split()
    if words[:1] == ["invoke"]:
        emit("ActionInvoked", (int(words[1]), words[2]))
    elif words[:1] == ["dismiss"]:
        emit("NotificationClosed", (int(words[1]), 2))
    return True


def acquired(bus, _name):
    global connection
    connection = bus
    record("ready")


bus = Gio.bus_get_sync(Gio.BusType.SESSION)
bus.register_object(PATH, Gio.DBusNodeInfo.new_for_xml(XML).interfaces[0], call, None, None)
Gio.bus_own_name_on_connection(bus, INTERFACE, Gio.BusNameOwnerFlags.NONE, acquired, None)
GLib.io_add_watch(GLib.IOChannel.unix_new(sys.stdin.fileno()), GLib.IO_IN | GLib.IO_HUP,
                  lambda channel, condition: command(sys.stdin, condition))
GLib.MainLoop().run()
