#!/usr/bin/env python3
"""Tables of the kept results (docs/speedtest.jsonl, wired and Wi-Fi rows), which
`tools/xectl.py bench ... --record` writes: one line for each set of conditions
and direction, the newest; git log -p on the file gives the older ones.

    results.py                 print both tables (Markdown)
    results.py --readme        rewrite them in README.md between the markers
                               <!-- results:wired --> ... <!-- /results:wired -->
                               (and the same for wifi), where those are present

Each row is one set of conditions -- card, how it is attached, the xnet settings,
the test and the number of streams -- with the newest result of each direction
and the date and commit it came from. A record carries its settings in full, so
nothing it says changes when defaults do.
"""

import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RESULTS = os.path.join(ROOT, "docs", "speedtest.jsonl")


def load(kind):
    if not os.path.exists(RESULTS):
        return []
    with open(RESULTS) as f:
        return [r for r in (json.loads(line) for line in f if line.strip()) if r["kind"] == kind]


def xnet_text(x):
    return "rcv %d / snd %d / cwnd %d KB, pool +%d KB, %d TCP sockets" % (
        x["rcvbuf_kb"], x["sndbuf_kb"], x["cwnd_kb"], x["pool_extra_kb"], x["tcp_sockets"])


def attach_text(r):
    if r["kind"] == "wifi":
        return "%s GHz, ch %d, %d MHz" % (r["band_ghz"], r["channel"], r["width_mhz"])
    return "%s, %d Mbit/s" % (r.get("transport", "-").upper(), r["link_mbps"])


def speed(r):
    return "%.0f Mbit/s (%.1f MB/s)" % (r["mbit_s"], r["mbyte_s"]) if r else "-"


def table(kind):
    rows = {}
    for r in load(kind):                       # later lines win
        key = (r["card"], attach_text(r), xnet_text(r["xnet"]), r["test"], r["streams"])
        rows.setdefault(key, {})[r["direction"]] = r
    head = ("| Card | %s | xnet | Test | Streams | Console -> PC | PC -> console | Date, commit | Note |"
            % ("Band, channel, width" if kind == "wifi" else "Transport, link"))
    out = [head, "|" + "---|" * 9]
    for key in sorted(rows, key=lambda k: (k[0], k[1], k[3], k[4], k[2])):
        d = rows[key]
        notes = "; ".join(sorted({r["note"] for r in d.values() if r.get("note")}))
        when = " / ".join("%s, %s" % (r["date"], r["commit"])
                          for r in (d.get("console->pc"), d.get("pc->console")) if r)
        out.append("| %s | %s | %s | %s | %d | %s | %s | %s | %s |" % (
            key[0], key[1], key[2], key[3], key[4], speed(d.get("console->pc")),
            speed(d.get("pc->console")), when, notes))
    return "\n".join(out)


def main(argv):
    tables = {kind: table(kind) for kind in ("wired", "wifi")}
    if "--readme" not in argv:
        for kind, text in tables.items():
            print("%s\n\n%s\n" % (kind, text))
        return 0
    path = os.path.join(ROOT, "README.md")
    text = open(path).read()
    for kind, body in tables.items():
        pattern = re.compile(r"(<!-- results:%s -->\n)(?:.*?\n)?(<!-- /results:%s -->)" % (kind, kind),
                             re.S)
        text = pattern.sub(lambda m: m.group(1) + body + "\n" + m.group(2), text)
    open(path, "w").write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
