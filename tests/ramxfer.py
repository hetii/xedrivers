#!/usr/bin/env python3
"""Console RAM to PC over the network, with no disk on either end: XBDM `getmemex`.

Reads a mapped region of console memory (xam's image by default) again and again
and reports the rate, for a range of request sizes and parallel connections. Every
byte comes out of the console's RAM through xnet and, in xnet mode, through the
RTL8156 -- so this is the network path's own throughput, not the hard disk's.

    ramxfer.py [--host H] [--mb 64] [--sizes 0x4000,0x10000,...] [--streams 1,2,4]

Uses xerunner's XBDM client (../xerunner/src), which reads the 2-byte chunk
framing of a 203 reply.
"""

import argparse
import os
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "..", "xerunner", "src"))
from xerunner.console.xbdm import Xbdm  # noqa: E402

REGION = (0x815F0000, 0x00600000)       # xam.xex as loaded: 6 MB, always mapped


def stream(host, size, total, out, index):
    base, span = REGION
    got = 0
    with Xbdm(host) as x:
        offset = (index * 0x100000) % span
        while got < total:
            want = min(size, total - got, span - offset)
            reply = x.command("getmemex addr=0x%08x length=0x%x" % (base + offset, want))
            if reply.code != 203 or len(reply.data) != want:
                raise SystemExit("getmemex: %d- %s" % (reply.code, reply.text))
            got += want
            offset = (offset + want) % span
    out[index] = got


def run(host, size, streams, total):
    out = [0] * streams
    per = total // streams
    threads = [threading.Thread(target=stream, args=(host, size, per, out, i))
               for i in range(streams)]
    t0 = time.time()
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    dt = time.time() - t0
    moved = sum(out)
    return moved * 8 / dt / 1e6, moved / dt / 1e6


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default=os.environ.get("XE_HOST", "192.168.1.210"))
    ap.add_argument("--mb", type=int, default=64)
    ap.add_argument("--sizes", default="0x4000,0x10000,0x40000,0x100000")
    ap.add_argument("--streams", default="1,2,4")
    a = ap.parse_args()
    total = a.mb << 20
    for streams in (int(s) for s in a.streams.split(",")):
        for size in (int(s, 0) for s in a.sizes.split(",")):
            try:
                mbit, mbyte = run(a.host, size, streams, total)
            except SystemExit as e:
                print("getmemex %7s x%d: %s" % (hex(size), streams, e))
                continue
            print("getmemex %7s x%d: %6.1f Mbit/s  %5.1f MB/s" % (
                hex(size), streams, mbit, mbyte))
    return 0


if __name__ == "__main__":
    sys.exit(main())
