#!/usr/bin/env python3
"""A minimal org.freedesktop.Notifications for tests. Prints "NOTIFY <id> <app> <title> <actions>" per Notify and
"CLOSE <id>" per CloseNotification; Invoke(u id, s key) emits ActionInvoked. Also prints "PORTAL_ACTION <app> <id>
<action>" for each org.freedesktop.impl.portal.Notification.ActionInvoked on the bus."""
from gi.repository import Gio, GLib

XML = """<node><interface name="org.freedesktop.Notifications">
<method name="Notify"><arg type="s" direction="in"/><arg type="u" direction="in"/><arg type="s" direction="in"/>
<arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="as" direction="in"/>
<arg type="a{sv}" direction="in"/><arg type="i" direction="in"/><arg type="u" direction="out"/></method>
<method name="CloseNotification"><arg type="u" direction="in"/></method>
<method name="Invoke"><arg type="u" direction="in"/><arg type="s" direction="in"/></method>
<signal name="ActionInvoked"><arg type="u"/><arg type="s"/></signal>
<signal name="NotificationClosed"><arg type="u"/><arg type="u"/></signal>
</interface></node>"""
next_id = [1]


def on_call(conn, sender, path, iface, method, params, invocation):
    if method == "Notify":
        app, replaces, _icon, title, _body, actions, _hints, _timeout = params.unpack()
        nid = replaces or next_id[0]
        next_id[0] += 1 if not replaces else 0
        print(f"NOTIFY {nid} {app} {title} {','.join(actions)}", flush=True)
        invocation.return_value(GLib.Variant("(u)", (nid,)))
    elif method == "CloseNotification":
        print(f"CLOSE {params.unpack()[0]}", flush=True)
        invocation.return_value(None)
    elif method == "Invoke":
        conn.emit_signal(None, path, iface, "ActionInvoked", params)
        invocation.return_value(None)


def on_portal_action(conn, sender, path, iface, signal, params):
    app, nid, action, _ = params.unpack()
    print(f"PORTAL_ACTION {app} {nid} {action}", flush=True)


bus = Gio.bus_get_sync(Gio.BusType.SESSION)
bus.register_object("/org/freedesktop/Notifications", Gio.DBusNodeInfo.new_for_xml(XML).interfaces[0], on_call)
bus.signal_subscribe(None, "org.freedesktop.impl.portal.Notification", "ActionInvoked", None, None,
                     Gio.DBusSignalFlags.NONE, on_portal_action)
Gio.bus_own_name_on_connection(bus, "org.freedesktop.Notifications", Gio.BusNameOwnerFlags.NONE, None, None)
GLib.MainLoop().run()
