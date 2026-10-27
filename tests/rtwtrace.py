#!/usr/bin/env python3
"""A Realtek USB Wi-Fi card's register traffic out of a usbmon pcap, one line each.

    rtwtrace.py capture.pcap DEVNUM [--from N] [--max N] [--bulk]

Register access on these cards (rtw88's usb.c) is vendor request 0x05 with the
address in wValue: bmRequestType 0x40 writes 1, 2 or 4 bytes, 0xc0 reads them.
Each line: index, time from the first, W/R, address, width, value (little-endian,
as the chip has it). With --bulk the bulk transfers are listed too, by endpoint and
length, with their first bytes. A recording of rtw88 bringing a card up is what a
port is checked against, write by write.
"""

import argparse
import struct


def packets(path):
    with open(path, "rb") as f:
        head = f.read(24)
        linktype = struct.unpack("<I", head[20:24])[0]
        hlen = 64 if linktype == 220 else 48
        while True:
            rec = f.read(16)
            if len(rec) < 16:
                return
            incl = struct.unpack("<IIII", rec)[2]
            data = f.read(incl)
            if len(data) < hlen:
                continue
            (urb, kind, xtype, ep, dev, bus, fsetup, fdata, sec, usec, status,
             length, caplen) = struct.unpack("<QBBBBhBBqiiII", data[:40])
            yield {"urb": urb, "kind": chr(kind), "type": xtype, "ep": ep, "dev": dev,
                   "status": status, "length": length,
                   "setup": data[40:48] if fsetup == 0 else None,
                   "data": data[hlen:hlen + caplen], "t": sec + usec / 1e6}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pcap")
    ap.add_argument("dev", type=int)
    ap.add_argument("--from", dest="start", type=int, default=0)
    ap.add_argument("--max", type=int, default=0)
    ap.add_argument("--bulk", action="store_true")
    a = ap.parse_args()

    pending = {}
    t0 = None
    n = 0
    shown = 0
    for p in packets(a.pcap):
        if p["dev"] != a.dev:
            continue
        if t0 is None:
            t0 = p["t"]
        if p["type"] == 2 and p["kind"] == "S" and p["setup"]:
            rtype, req, value, index, length = struct.unpack("<BBHHH", p["setup"])
            if req != 0x05 or rtype not in (0x40, 0xC0):
                continue
            pending[p["urb"]] = (rtype, value, length, p["data"], p["t"])
            continue
        if p["type"] == 2 and p["kind"] == "C" and p["urb"] in pending:
            rtype, addr, length, out, t = pending.pop(p["urb"])
            raw = out if rtype == 0x40 else p["data"]
            value = int.from_bytes(raw[:length], "little") if raw else None
            if n >= a.start and (not a.max or shown < a.max):
                print("%6d %9.4f %s %04x/%d %s%s" % (
                    n, t - t0, "W" if rtype == 0x40 else "R", addr, length,
                    "%0*x" % (2 * length, value) if value is not None else "?",
                    "" if p["status"] == 0 else " status=%d" % p["status"]))
                shown += 1
            n += 1
            continue
        if a.bulk and p["type"] == 3 and n >= a.start and (not a.max or shown < a.max):
            if (p["kind"] == "S" and not p["ep"] & 0x80) or \
               (p["kind"] == "C" and p["ep"] & 0x80):
                print("%6s %9.4f B ep%02x len=%d %s" % (
                    "", p["t"] - t0, p["ep"], p["length"], p["data"][:24].hex()))
                shown += 1


if __name__ == "__main__":
    main()
