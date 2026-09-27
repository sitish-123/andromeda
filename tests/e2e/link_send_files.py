#!/usr/bin/env python3
"""Calls org.umbriel.Link1.SendFiles with open descriptors, as the shell does, and prints the transfer id.

Usage: link_send_files.py DEVICE_ID PATH[=NAME]...
"""
import os
import sys

from gi.repository import Gio, GLib


def main() -> int:
    device, specs = sys.argv[1], sys.argv[2:]
    fds = Gio.UnixFDList()
    files = []
    for spec in specs:
        path, _, name = spec.partition("=")
        files.append((fds.append(os.open(path, os.O_RDONLY)), name or os.path.basename(path)))
    bus = Gio.bus_get_sync(Gio.BusType.SESSION)
    try:
        reply, _ = bus.call_with_unix_fd_list_sync(
            "org.umbriel.Link1", "/org/umbriel/Link1", "org.umbriel.Link1", "SendFiles",
            GLib.Variant("(sa(hs))", (device, files)), GLib.VariantType("(s)"), Gio.DBusCallFlags.NONE, -1, fds,
            None)
    except GLib.Error as error:
        print(error.message, file=sys.stderr)
        return 1
    print(reply.unpack()[0])
    return 0


if __name__ == "__main__":
    sys.exit(main())
