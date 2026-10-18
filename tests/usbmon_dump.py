#!/usr/bin/env python3
"""Print one USB device's control requests and bulk data out of a usbmon pcap.

    usbmon_dump.py capture.pcap DEVNUM [--bulk-out EP] [--max N]

Handles both usbmon link types (189 USB_LINUX, 48-byte header; 220
USB_LINUX_MMAPPED, 64-byte header). Control transfers are shown as their submit
(setup packet, OUT data) joined with their completion (status, IN data); bulk OUT
as the data submitted.
"""

import argparse
import struct
import sys


def packets(path):
    with open(path, "rb") as f:
        head = f.read(24)
        magic = struct.unpack("<I", head[:4])[0]
        endian = "<" if magic in (0xA1B2C3D4, 0xA1B23C4D) else ">"
        linktype = struct.unpack(endian + "I", head[20:24])[0]
        hlen = 64 if linktype == 220 else 48
        while True:
            rec = f.read(16)
            if len(rec) < 16:
                return
            _, _, incl, _ = struct.unpack(endian + "IIII", rec)
            data = f.read(incl)
            if len(data) < hlen:
                continue
            (urb, kind, xtype, ep, dev, bus, fsetup, fdata, sec, usec, status,
             length, caplen) = struct.unpack("<QBBBBhBBqiiII", data[:40])
            setup = data[40:48]
            yield {"urb": urb, "kind": chr(kind), "type": xtype, "ep": ep,
                   "dev": dev, "status": status, "length": length,
                   "setup": setup if fsetup == 0 else None,
                   "data": data[hlen:hlen + caplen], "t": sec + usec / 1e6}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pcap")
    ap.add_argument("dev", type=int)
    ap.add_argument("--bulk-out", type=lambda s: int(s, 0), default=0x02)
    ap.add_argument("--max", type=int, default=40)
    a = ap.parse_args()
    pending = {}
    shown = 0
    t0 = None
    for p in packets(a.pcap):
        if p["dev"] != a.dev:
            continue
        t0 = t0 if t0 is not None else p["t"]
        when = p["t"] - t0
        if p["type"] == 2:                              # control
            if p["kind"] == "S":
                pending[p["urb"]] = p
            elif p["kind"] == "C" and p["urb"] in pending:
                s = pending.pop(p["urb"])
                setup = s["setup"] or b"\0" * 8
                out = s["data"] if not setup[0] & 0x80 else b""
                inn = p["data"] if setup[0] & 0x80 else b""
                print("%8.3f CTRL %s status=%d len=%d out=%s in=%s" % (
                    when, setup.hex(" "), p["status"], p["length"],
                    out[:32].hex(), inn[:32].hex()))
        elif p["type"] == 3 and p["kind"] == "S" and p["ep"] == a.bulk_out:
            if shown < a.max:
                print("%8.3f BULK OUT %d bytes: %s" % (when, p["length"],
                                                       p["data"][:96].hex(" ")))
                shown += 1
        elif p["type"] == 3 and p["kind"] == "C" and p["ep"] == a.bulk_out:
            if shown <= a.max:
                print("%8.3f   done status=%d len=%d" % (when, p["status"],
                                                         p["length"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
