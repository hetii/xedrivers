#!/usr/bin/env python3
"""Drive a running xedrivers plugin from the host, over XBDM.

Every global the plugin exposes sits at a fixed address (the image is linked at a
fixed base, no relocations), so this reads them out of build/plugin.elf with nm and
talks to the console through xerunner's `console read-memory` / `write-memory`.

    xectl.py status                     state, config, link, counters
    xectl.py mode standalone|xnet|drop  who gets the frames
    xectl.py ip A.B.C.D                 standalone address (reselects standalone)
    xectl.py transport ecm|ncm          USB transport; resets the card to apply
    xectl.py set|get SYMBOL [VALUE]     any 32-bit global, e.g. net_tx_eager
    xectl.py advertise 2500|1000|100|default
    xectl.py phy 0xa400 [value]         read/write a PHY register (OCP address)
    xectl.py reg pla|usb 0xe610 [value] read/write a MAC register word
    xectl.py blast COUNT [PAYLOAD] [PORT]
    xectl.py cmd N [a1 a2 a3]           any raw command

Environment: XE_HOST (default 192.168.1.210), XERUNNER (path to the CLI).
"""

import json
import os
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ELF = os.path.join(ROOT, "build", "plugin.elf")
HOST = os.environ.get("XE_HOST", "192.168.1.210")
XERUNNER = os.environ.get(
    "XERUNNER", os.path.join(ROOT, "..", "xerunner", ".venv", "bin", "xerunner"))

CMD = {"ping": 1, "reset": 2, "claim": 3, "blast": 4, "mode": 5, "restart": 6,
       "advertise": 7, "reg_read": 8, "reg_write": 9, "phy_read": 10, "phy_write": 11,
       "xnet_iface": 13}
MODES = {"drop": 0, "standalone": 1, "xnet": 2}
ADVERT = {"2500": 0x6F, "1000": 0x2F, "100": 0x0F, "default": 0}
MCU = {"pla": 0x0100, "usb": 0x0000}

STRUCTS = {
    "xe_state": ["magic", "version", "worker_ticks", "claim_attempts", "claim_found",
                 "start_result", "start_count", "mode", "xnet_status",
                 "advertise_status", "chip_version"],
    "xe_config": ["magic", "mode", "advertise", "ip", "transport"],
    "net_stats": ["step", "step_status", "ready", "link", "speed_mbps", "rx_frames",
                  "rx_bytes", "rx_errors", "rx_last_error", "rx_short", "tx_frames",
                  "tx_bytes", "tx_drops", "tx_errors", "tx_last_error", "tx_inflight",
                  "intr_events", "intr_errors", "last_notify0", "last_notify1",
                  "rx_stopped", "config_value", "data_interface", "ep_in", "ep_out",
                  "ep_intr", "rx_bytes_hi", "tx_bytes_hi", "transport", "rx_ntbs",
                  "rx_ntb_errors", "tx_ntbs", "ntb_in_max", "ntb_out_max",
                  "ntb_out_align", "ntb_out_datagrams", "ntb_input_status",
                  "ntb_format_status", "crc_status", "dgram_status", "ntb_formats"],
    "stack_stats": ["arp_replies", "icmp_replies", "udp_echo", "discard_frames",
                    "discard_bytes", "discard_bytes_hi", "peer_ip", "peer_mac_hi",
                    "peer_mac_lo", "blast_sent", "blast_busy", "not_ours"],
    "xnet_stats": ["enabled", "patched", "user", "xmit_usb", "xmit_fallback",
                   "rx_delivered", "rx_filtered", "rx_no_user", "link_polls",
                   "flags_before", "flags_after", "link_notifies", "xmit_dropped",
                   "xq_high"],
}


def symbols():
    out = subprocess.run(["powerpc-linux-gnu-nm", ELF], capture_output=True,
                         text=True, check=True).stdout
    table = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3:
            table[parts[2]] = int(parts[0], 16)
    return table


def xerunner(group, sub, *positional):
    done = subprocess.run([XERUNNER, group, sub, "--host", HOST, "--json", "--",
                           *positional], capture_output=True, text=True)
    if done.returncode:
        raise SystemExit("xerunner %s failed: %s" % (sub, done.stdout + done.stderr))
    return json.loads(done.stdout)


def read(addr, length):
    got = xerunner("console", "read-memory", hex(addr), hex(length))
    return bytes.fromhex((got.get("output") or got)["data"])


def write(addr, data):
    xerunner("console", "write-memory", hex(addr), data.hex())


def read_struct(sym, name):
    fields = STRUCTS[name]
    raw = read(sym[name], 4 * len(fields))
    return dict(zip(fields, struct.unpack(">%dI" % len(fields), raw)))


def command(sym, what, *args, wait=30.0):
    """Arguments first, the request word last: the worker takes the request the
    moment it sees it, so it must not see it before the arguments are in place."""
    base = sym["xe_cmd"]
    before = struct.unpack(">I", read(base + 16, 4))[0]
    words = list(args) + [0] * (3 - len(args))
    write(base + 4, struct.pack(">3I", *words))
    write(base, struct.pack(">I", what))
    deadline = time.time() + wait
    while time.time() < deadline:
        ack, result, answered, status = struct.unpack(">4I", read(base + 16, 16))
        if ack != before and answered == what:
            return result, status
        time.sleep(0.2)
    raise SystemExit("no answer to command %d within %.0fs" % (what, wait))


def ip(v):
    return "%d.%d.%d.%d" % (v >> 24, (v >> 16) & 255, (v >> 8) & 255, v & 255)


def status(sym):
    st = read_struct(sym, "xe_state")
    cf = read_struct(sym, "xe_config")
    ns = read_struct(sym, "net_stats")
    ss = read_struct(sym, "stack_stats")
    xs = read_struct(sym, "xnet_stats")
    mac = read(sym["net_mac"], 6)
    names = {v: k for k, v in MODES.items()}
    print("plugin   magic=%08x version=%08x ticks=%d chip=%04x" % (
        st["magic"], st["version"], st["worker_ticks"], st["chip_version"]))
    print("config   mode=%s advertise=0x%02x ip=%s" % (
        names.get(cf["mode"], cf["mode"]), cf["advertise"], ip(cf["ip"])))
    print("running  mode=%s start=%d/%#x adv_status=%#x claims=%d/%d" % (
        names.get(st["mode"], st["mode"]), st["start_count"], st["start_result"],
        st["advertise_status"], st["claim_found"], st["claim_attempts"]))
    print("card     ready=%d link=%d speed=%dM mac=%s config=%d transport=%s step=%d/%#x" % (
        ns["ready"], ns["link"], ns["speed_mbps"], mac.hex(":"), ns["config_value"],
        "ncm" if ns["transport"] else "ecm", ns["step"], ns["step_status"]))
    if ns["transport"]:
        print("ntb      in_max=%d out_max=%d align=%#x max_dg=%d formats=%#x rx=%d "
              "rx_err=%d tx=%d" % (
                  ns["ntb_in_max"], ns["ntb_out_max"], ns["ntb_out_align"],
                  ns["ntb_out_datagrams"], ns["ntb_formats"], ns["rx_ntbs"],
                  ns["rx_ntb_errors"], ns["tx_ntbs"]))
        print("ncm req  set_input=%#x set_format=%#x crc_off=%#x max_dgram=%#x" % (
            ns["ntb_input_status"], ns["ntb_format_status"], ns["crc_status"],
            ns["dgram_status"]))
    print("rx       frames=%d bytes=%d errors=%d stopped=%d" % (
        ns["rx_frames"], ns["rx_bytes_hi"] << 32 | ns["rx_bytes"], ns["rx_errors"],
        ns["rx_stopped"]))
    print("tx       frames=%d bytes=%d drops=%d errors=%d" % (
        ns["tx_frames"], ns["tx_bytes_hi"] << 32 | ns["tx_bytes"], ns["tx_drops"],
        ns["tx_errors"]))
    print("stack    arp=%d icmp=%d echo=%d discard=%d/%dB peer=%s blast=%d busy=%d" % (
        ss["arp_replies"], ss["icmp_replies"], ss["udp_echo"], ss["discard_frames"],
        ss["discard_bytes_hi"] << 32 | ss["discard_bytes"], ip(ss["peer_ip"]),
        ss["blast_sent"], ss["blast_busy"]))
    print("xnet     enabled=%d user=%08x xmit usb/fallback=%d/%d rx=%d filtered=%d "
          "polls=%d notifies=%d flags %#x->%#x" % (
              xs["enabled"], xs["user"], xs["xmit_usb"], xs["xmit_fallback"],
              xs["rx_delivered"], xs["rx_filtered"], xs["link_polls"],
              xs["link_notifies"], xs["flags_before"], xs["flags_after"]))
    print("xnet tx  queued_high=%d dropped=%d" % (xs["xq_high"], xs["xmit_dropped"]))


def main(argv):
    if not argv:
        print(__doc__)
        return 1
    sym = symbols()
    what, rest = argv[0], argv[1:]
    num = lambda s: int(s, 0)
    if what == "status":
        status(sym)
    elif what == "transport":
        # Read at bring-up only: write it, then reset the card so it re-enumerates.
        value = {"ecm": 0, "ncm": 1}[rest[0]]
        write(sym["xe_config"] + 16, struct.pack(">I", value))
        print("reset %#x" % command(sym, CMD["reset"])[0])
    elif what == "set":
        # Any 32-bit global by name, e.g. `set net_tx_eager 4`.
        write(sym[rest[0]], struct.pack(">I", num(rest[1])))
        print("%s = %#x" % (rest[0], struct.unpack(">I", read(sym[rest[0]], 4))[0]))
    elif what == "get":
        print("%s = %#x" % (rest[0], struct.unpack(">I", read(sym[rest[0]], 4))[0]))
    elif what == "ip":
        # The standalone address lives in xe_config and is applied when the mode
        # is (re)selected, so write it and select standalone again.
        a, b, c, d = (int(x) for x in rest[0].split("."))
        write(sym["xe_config"] + 12, struct.pack(">I", a << 24 | b << 16 | c << 8 | d))
        print("result %#x status %#x" % command(sym, CMD["mode"], MODES["standalone"]))
    elif what == "mode":
        print("result %#x status %#x" % command(sym, CMD["mode"], MODES[rest[0]]))
    elif what == "advertise":
        print("result %#x" % command(sym, CMD["advertise"], ADVERT[rest[0]])[0])
    elif what == "phy":
        if len(rest) == 1:
            value, st = command(sym, CMD["phy_read"], num(rest[0]))
            print("%04x (status %#x)" % (value, st))
        else:
            print("status %#x" % command(sym, CMD["phy_write"], num(rest[0]),
                                         num(rest[1]))[0])
    elif what == "reg":
        mcu = MCU[rest[0]]
        if len(rest) == 2:
            value, st = command(sym, CMD["reg_read"], mcu, num(rest[1]))
            print("%04x (status %#x)" % (value, st))
        else:
            print("status %#x" % command(sym, CMD["reg_write"], mcu, num(rest[1]),
                                         num(rest[2]))[0])
    elif what == "blast":
        count = num(rest[0])
        payload = num(rest[1]) if len(rest) > 1 else 1472
        port = num(rest[2]) if len(rest) > 2 else 5001
        print("sent %d" % command(sym, CMD["blast"], count, payload, port,
                                  wait=600)[0])
    elif what == "cmd":
        print("result %#x status %#x" % command(sym, num(rest[0]),
                                                *[num(x) for x in rest[1:]]))
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
