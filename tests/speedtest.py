#!/usr/bin/env python3
"""Throughput and latency of the console's RTL8156 in standalone mode.

Directions, each timed on the side that receives:

  tx   console -> PC: the plugin's UDP generator (stack_blast) sends; the rate is
       the PC card's rx_bytes counter over the run.
  rx   PC -> console: this host floods UDP to 10.36.0.2:9 (discard); the rate is
       the plugin's discard byte counter, read before and after over XBDM.
  ping ICMP round trip.

    speedtest.py [tx|rx|ping|all] [--seconds N] [--payload B]
"""

import argparse
import os
import socket
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xectl  # noqa: E402

IFACE = os.environ.get("XE_IFACE", "enx00e04c68012a")
CONSOLE_IP = os.environ.get("XE_IP", "10.36.0.2")
LOCAL_IP = os.environ.get("XE_LOCAL", "10.36.0.1")


def nic(counter):
    with open("/sys/class/net/%s/statistics/%s" % (IFACE, counter)) as f:
        return int(f.read())


def link_speed():
    with open("/sys/class/net/%s/speed" % IFACE) as f:
        return int(f.read())


def test_ping():
    out = subprocess.run(["ping", "-c", "20", "-i", "0.2", "-q", CONSOLE_IP],
                         capture_output=True, text=True).stdout
    line = [l for l in out.splitlines() if "rtt" in l or "loss" in l]
    print("ping      " + " | ".join(line))


def test_tx(sym, seconds, payload):
    # The plugin sends to whoever spoke to it last; a ping makes that us.
    subprocess.run(["ping", "-c", "1", "-W", "1", CONSOLE_IP], capture_output=True)
    xectl.command(sym, xectl.CMD["blast"], 0xFFFFFFFF, payload, 5001)
    # Sample a steady window, so the XBDM round trips that start and stop the run
    # are not counted as transfer time.
    time.sleep(1)
    b1, p1, t1 = nic("rx_bytes"), nic("rx_packets"), time.time()
    time.sleep(seconds)
    b2, p2, t2 = nic("rx_bytes"), nic("rx_packets"), time.time()
    sent = xectl.command(sym, xectl.CMD["blast"], 0)[0]
    ns = xectl.read_struct(sym, "net_stats")
    rate = (b2 - b1) * 8 / (t2 - t1) / 1e6
    print("tx        console->PC %.1f Mbit/s, %.0f pkt/s, payload %d, sent %d, "
          "tx errors %d" % (rate, (p2 - p1) / (t2 - t1), payload, sent, ns["tx_errors"]))
    return rate


def _flood(seconds, payload, out):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 8 << 20)
    s.bind((LOCAL_IP, 0))
    s.connect((CONSOLE_IP, 9))
    data = bytes(payload)
    sent = 0
    end = time.time() + seconds
    while time.time() < end:
        for _ in range(256):
            try:
                s.send(data)
                sent += 1
            except OSError:
                time.sleep(0.0001)
    out.put(sent)


def test_rx(sym, seconds, payload, senders):
    import multiprocessing as mp
    st0 = xectl.read_struct(sym, "stack_stats")
    ns0 = xectl.read_struct(sym, "net_stats")
    q = mp.Queue()
    procs = [mp.Process(target=_flood, args=(seconds, payload, q))
             for _ in range(senders)]
    t0 = time.time()
    for p in procs:
        p.start()
    for p in procs:
        p.join()
    t1 = time.time()
    sent = sum(q.get() for _ in procs)
    time.sleep(0.5)
    st1 = xectl.read_struct(sym, "stack_stats")
    ns1 = xectl.read_struct(sym, "net_stats")
    got = ((st1["discard_bytes_hi"] << 32 | st1["discard_bytes"]) -
           (st0["discard_bytes_hi"] << 32 | st0["discard_bytes"]))
    frames = st1["discard_frames"] - st0["discard_frames"]
    offered = sent * payload * 8 / (t1 - t0) / 1e6
    rate = got * 8 / (t1 - t0) / 1e6
    print("rx        PC->console %.1f Mbit/s received (offered %.1f, %d sender%s), "
          "%d/%d datagrams (%.2f%% lost), rx errors +%d" % (
              rate, offered, senders, "s" if senders > 1 else "", frames, sent,
              100.0 * (sent - frames) / max(sent, 1),
              ns1["rx_errors"] - ns0["rx_errors"]))
    return rate


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("which", nargs="?", default="all", choices=["tx", "rx", "ping", "all"])
    ap.add_argument("--seconds", type=float, default=10)
    ap.add_argument("--payload", type=int, default=1472)
    ap.add_argument("--senders", type=int, default=1)
    a = ap.parse_args()
    sym = xectl.symbols()
    print("link      %d Mbit/s on %s" % (link_speed(), IFACE))
    if a.which in ("ping", "all"):
        test_ping()
    if a.which in ("tx", "all"):
        test_tx(sym, a.seconds, a.payload)
    if a.which in ("rx", "all"):
        test_rx(sym, a.seconds, a.payload, a.senders)
    return 0


if __name__ == "__main__":
    sys.exit(main())
