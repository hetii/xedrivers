#!/usr/bin/env python3
"""Dump the console kernel's USB device tree straight from its tables, over XBDM.

The kernel keeps, per controller pair, a table of 16-byte nodes (driver words,
controller/port byte, parent/child/sibling links) and four shared descriptor
slots. Addresses are those of retail 17559 (read off UsbdGetRootHubDeviceNode,
UsbdGetPortDeviceNode and UsbdGetDeviceDescriptor); verify them on another
kernel first. A node's w1 is the driver object that owns it.

    XE_HOST=192.168.1.210 tests/usbtree.py
"""
import sys, struct
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xectl  # noqa: E402
rd = lambda a, n: xectl.read(a, n)
ctrls = struct.unpack(">4I", rd(0x801A8230, 16))
slots = struct.unpack(">4I", rd(0x801A8220, 16))
print("descriptor slots:")
for i, s in enumerate(slots):
    d = rd(s + 76, 18)
    print("  slot %d %08x  %04x:%04x class %02x" % (i, s, struct.unpack("<H", d[8:10])[0], struct.unpack("<H", d[10:12])[0], d[4]))
seen = set()
for ci, c in enumerate(ctrls):
    hdr = struct.unpack(">20I", rd(c, 80))
    nodes = hdr[16]
    print("controller %d at %08x vtable %08x nodes %08x" % (ci, c, hdr[0], nodes))
    if nodes in seen:
        continue
    seen.add(nodes)
    raw = rd(nodes, 32 * 16)
    for i in range(32):
        n = raw[16 * i:16 * i + 16]
        w0, w1 = struct.unpack(">2I", n[:8])
        if not any(n[8:]) and not w1:
            continue
        b = n[8:]
        print("  node %2d w0=%08x w1=%08x ctrl=%d port=%d b8=%02x b9=%02x b10=%02x b11=%02x(type %d slot %d) parent=%02x child=%02x sib=%02x"
              % (i, w0, w1, b[0] & 3, (b[0] >> 2) & 7, b[0], b[1], b[2], b[3], b[3] & 7, (b[3] >> 3) & 3, b[5], b[6], b[7]))
