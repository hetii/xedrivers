#!/usr/bin/env python3
"""TCP throughput against the plugin's own server (src/bench/tcp/), RAM to RAM.

    tcpbench.py [--host H] [--seconds S] [--streams 1,2,4] [--pace MB/s] [down|up|both]

down  console -> PC: read from port 5201, which sends out of a RAM buffer
up    PC -> console: write to port 5202, which reads and discards

The rate is the bytes moved over a steady window, measured on this side; the
first half second of each run is left out, so connection setup and slow start do
not count as transfer time. --pace caps each stream this side sends (Linux
SO_MAX_PACING_RATE), so it goes out evenly instead of in bursts.
"""

import argparse
import os
import socket
import sys
import threading
import time

WARMUP = 0.5
SO_MAX_PACING_RATE = 47


def stream(host, port, seconds, out, index, up, pace=0):
    s = socket.create_connection((host, port), timeout=10)
    if pace:
        s.setsockopt(socket.SOL_SOCKET, SO_MAX_PACING_RATE, int(pace * 1e6))
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF if not up else
                 socket.SO_SNDBUF, 4 << 20)
    block = bytes(256 << 10) if up else None
    counted = 0
    start = time.time()
    begin = start + WARMUP
    end = begin + seconds
    try:
        while True:
            now = time.time()
            if now >= end:
                break
            n = s.send(block) if up else len(s.recv(1 << 20))
            if n <= 0:
                break
            if now >= begin:
                counted += n
    finally:
        s.close()
    out[index] = counted


def run(host, port, streams, seconds, up, pace=0):
    out = [0] * streams
    threads = [threading.Thread(target=stream,
                                args=(host, port, seconds, out, i, up, pace))
               for i in range(streams)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    total = sum(out)
    return total / seconds / 1e6, total * 8 / seconds / 1e6


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("which", nargs="?", default="both", choices=["down", "up", "both"])
    ap.add_argument("--host", default=os.environ.get("XE_HOST", "192.168.1.210"))
    ap.add_argument("--seconds", type=float, default=8)
    ap.add_argument("--streams", default="1,2,4")
    ap.add_argument("--pace", type=float, default=0, help="MB/s per stream sent")
    a = ap.parse_args()
    for n in (int(x) for x in a.streams.split(",")):
        if a.which in ("down", "both"):
            mb, mbit = run(a.host, 5201, n, a.seconds, False)
            print("down x%d  console->PC  %5.1f MB/s  %6.1f Mbit/s" % (n, mb, mbit))
        if a.which in ("up", "both"):
            mb, mbit = run(a.host, 5202, n, a.seconds, True, a.pace)
            print("up   x%d  PC->console  %5.1f MB/s  %6.1f Mbit/s" % (n, mb, mbit))
    return 0


if __name__ == "__main__":
    sys.exit(main())
