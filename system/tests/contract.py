#!/usr/bin/env python3
"""Merges introspection XML documents (argv) into one <node> with each org.andromeda.System1.* interface once,
sorted by name: the committed contract system/data/org.andromeda.System1.xml."""
import sys
import xml.etree.ElementTree as ET

interfaces = {}
for path in sys.argv[1:]:
    for iface in ET.parse(path).getroot().iter("interface"):
        name = iface.get("name", "")
        if name.startswith("org.andromeda.System1."):
            interfaces[name] = iface
root = ET.Element("node")
for name in sorted(interfaces):
    root.append(interfaces[name])
ET.indent(root)
sys.stdout.write('<!DOCTYPE node PUBLIC "-//freedesktop//DTD D-BUS Object Introspection 1.0//EN"\n'
                 ' "http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd">\n')
sys.stdout.write(ET.tostring(root, encoding="unicode") + "\n")
