#!/usr/bin/env python3
"""Console -> phone throughput over RNDIS, measured on the console.

In standalone mode, with the phone tethering, the plugin's UDP generator sends to
the broadcast address of the phone's network (the phone takes it and discards it);
the rate is the bytes the USB OUT endpoint accepted over the run, from the
console's own counters and clock. No peer on the phone side is needed.

    XE_HOST=10.36.0.85 phoneblast.py [--count N] [--payload B] [--net 10.63.8.255]
"""

import argparse
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xectl  # noqa: E402


def counters(sym):
    ns = xectl.read_struct(sym, "net_stats")
    stamp = struct.unpack(">I", xectl.read(sym["net_prof"] + 8, 4))[0] \
        if "net_prof" in sym else None
    return ns, stamp, time.time()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--count", type=int, default=200000)
    ap.add_argument("--payload", type=int, default=1472)
    ap.add_argument("--net", default="10.63.8.255")
    a = ap.parse_args()
    sym = xectl.symbols()
    ip = [int(x) for x in a.net.split(".")]
    xectl.write(sym["peer_mac"], b"\xff" * 6)
    xectl.write(sym["stack_stats"] + 4 * xectl.STRUCTS["stack_stats"].index("peer_ip"),
                struct.pack(">I", ip[0] << 24 | ip[1] << 16 | ip[2] << 8 | ip[3]))
    xectl.command(sym, xectl.CMD["blast"], a.count, a.payload, 9)
    time.sleep(1.0)
    n0, s0, t0 = counters(sym)
    time.sleep(5.0)
    n1, s1, t1 = counters(sym)
    xectl.command(sym, xectl.CMD["blast"], 0, 0, 0)
    secs = ((s1 - s0) & 0xFFFFFFFF) / 50e6 if s0 is not None else t1 - t0
    moved = (n1["tx_bytes_hi"] << 32 | n1["tx_bytes"]) - (n0["tx_bytes_hi"] << 32 |
                                                          n0["tx_bytes"])
    frames = n1["tx_frames"] - n0["tx_frames"]
    print("console->phone  %.1f MB/s  %.0f frames/s  (%d transfers' bytes over %.2f s, "
          "errors %d, drops %d)" % (moved / secs / 1e6, frames / secs, moved, secs,
                                    n1["tx_errors"] - n0["tx_errors"],
                                    n1["tx_drops"] - n0["tx_drops"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
