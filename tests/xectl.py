#!/usr/bin/env python3
"""Drive a running xedrivers plugin from the host, over XBDM.

Every global the plugin exposes sits at a fixed address (the image is linked at a
fixed base, no relocations), so this reads them out of build/plugin.elf with nm and
talks to the console through xerunner's `console read-memory` / `write-memory`.

    xectl.py status                     state, config, link, counters
    xectl.py mode standalone|xnet|drop  who gets the frames
    xectl.py ip A.B.C.D                 standalone address (reselects standalone)
    xectl.py transport ecm|ncm          USB transport; resets the card to apply
    xectl.py cmd 14                     start the TCP throughput server
    xectl.py set|get SYMBOL [VALUE]     any 32-bit global, e.g. cdc_tx_eager
    xectl.py advertise 2500|1000|100|default
    xectl.py phy 0xa400 [value]         read/write a PHY register (OCP address)
    xectl.py reg pla|usb 0xe610 [value] read/write a MAC register word
    xectl.py blast COUNT [PAYLOAD] [PORT]
    xectl.py cmd N [a1 a2 a3]           any raw command
    xectl.py prof [SECONDS]             where a frame's time goes (bench=prof)
    xectl.py history [N]                the last N flight-recorder samples (200 ms)

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
       "xnet_iface": 13, "tcp_start": 14}
MODES = {"drop": 0, "standalone": 1, "xnet": 2}
ADVERT = {"2500": 0x6F, "1000": 0x2F, "100": 0x0F, "default": 0}
MCU = {"pla": 0x0100, "usb": 0x0000}

PROF_SLOTS = ['copy', 'submit', 'tx_done', 'rx_done', 'rx_up', 'tx_wait', 'turnaround']

STRUCTS = {
    "net_prof": ["enabled", "freq", "stamp", "copy_ticks_hi", "copy_ticks_lo", "copy_bytes_hi", "copy_bytes_lo", "copy_calls", "submit_ticks_hi", "submit_ticks_lo", "submit_bytes_hi", "submit_bytes_lo", "submit_calls", "tx_done_ticks_hi", "tx_done_ticks_lo", "tx_done_bytes_hi", "tx_done_bytes_lo", "tx_done_calls", "rx_done_ticks_hi", "rx_done_ticks_lo", "rx_done_bytes_hi", "rx_done_bytes_lo", "rx_done_calls", "rx_up_ticks_hi", "rx_up_ticks_lo", "rx_up_bytes_hi", "rx_up_bytes_lo", "rx_up_calls", "tx_wait_ticks_hi", "tx_wait_ticks_lo", "tx_wait_bytes_hi", "tx_wait_bytes_lo", "tx_wait_calls", "turnaround_ticks_hi", "turnaround_ticks_lo", "turnaround_bytes_hi", "turnaround_bytes_lo", "turnaround_calls"],
    "xe_state": ["magic", "version", "worker_ticks", "claim_attempts", "claim_found"],
    "net_state": ["start_result", "start_count", "mode", "xnet_status",
                  "console_mac_status", "console_mac_needed", "console_mac_read0",
                  "console_mac_read1", "console_mac_from_config"],
    "xe_config": ["magic", "mode", "advertise", "ip", "transport"],
    "net_stats": ["driver", "step", "step_status", "ready", "link", "speed_mbps",
                  "rx_frames", "rx_bytes", "rx_bytes_hi", "rx_errors", "rx_last_error",
                  "rx_short", "rx_stopped", "tx_frames", "tx_bytes", "tx_bytes_hi",
                  "tx_drops", "tx_errors", "tx_last_error", "tx_inflight",
                  "rx_not_uplink", "where"],
    "cdc_stats": ["transport", "config_value", "data_interface", "ep_in", "ep_out",
                  "ep_intr", "intr_events", "intr_errors", "last_notify0",
                  "last_notify1", "rx_deferred", "rx_ntbs", "rx_ntb_errors", "tx_ntbs",
                  "ntb_in_max", "ntb_out_max", "ntb_out_align", "ntb_out_datagrams",
                  "ntb_formats", "ntb_input_status", "ntb_format_status", "crc_status",
                  "dgram_status", "intr_parked"],
    "rtl_stats": ["chip_version", "advertise_status"],
    "rndis_stats": ["init_status", "max_packets", "max_transfer", "alignment",
                    "filter_status", "media_status", "link_speed", "control_polls",
                    "keepalives", "indications", "last_indication", "rx_transfers",
                    "rx_bad", "intr_events", "intr_errors", "tx_too_big",
                    "tx_transfers"],
    "aic_stats": ["product", "step", "step_status", "chip_word", "chip_id", "mcu_id",
                  "fw_version", "bytes_loaded", "files_loaded",
                  "file_status", "commands", "timeouts", "tx_errors",
                  "tx_last_error", "rx_transfers", "rx_errors",
                  "rx_last_error", "rx_messages", "rx_unexpected",
                  "rx_last_id", "rx_data", "last_cfm_id", "rx_first0",
                  "rx_first1", "rx_first2", "rx_first3", "rx_first4",
                  "rx_first5", "rx_first6", "rx_first7", "scan_results",
                  "fw_text0", "fw_text1", "fw_text2", "fw_text3",
                  "fw_text4", "fw_text5", "fw_text6", "fw_text7",
                  "fw_text8", "fw_text9", "fw_text10", "fw_text11",
                  "connects", "connect_status", "connect_result",
                  "joined", "disconnects", "disconnect_reason", "ap_sta",
                  "aid", "join_freq", "join_refused_security",
                  "secured_joins", "eapol_in", "eapol_out",
                  "eapol_dropped", "keys_added", "key_status",
                  "port_status", "vif", "if_adds", "if_add_status",
                  "if_removes", "if_remove_status", "if_mac0", "if_mac1",
                  "join_index", "join_freq_asked", "ps_off_status",
                  "auto_scans", "auto_joins", "auto_missed",
                  "auto_secured", "bt_open_status", "bt_opened", "bssid0",
                  "bssid1", "data_tx_frames", "data_tx_full",
                  "data_tx_errors", "data_tx_last_error",
                  "data_rx_transfers", "data_rx_frames", "data_rx_errors",
                  "data_rx_last_error", "data_rx_not_data",
                  "data_rx_unsupported", "data_rx_not_from_ds",
                  "data_rx_short", "data_rx_truncated",
                  "data_rx_out_of_order", "reorder_held", "reorder_late",
                  "reorder_timeouts", "reorder_full"],
    "wpa_stats": ["state", "failures", "replays", "mic_failures", "messages_in",
                  "messages_out", "unwrap_failures", "last_key_info", "gtk_len",
                  "no_gtk", "pending", "applied", "psk_ready"],
    "aic_bt_stats": ["up", "commands", "command_errors", "timeouts", "events",
                     "event_errors", "last_event", "last_event_error",
                     "last_opcode", "reset_status", "version_status",
                     "address_status", "hci_version", "hci_revision",
                     "lmp_version", "manufacturer", "address0", "address1"],
    "wl_stats": ["scans", "scans_done", "results", "bad_frames", "table_full",
                 "bss_count", "last_scan_status"],
    "file_stats": ["opens", "found", "last_status", "read_errors",
                   "last_read_status"],
    "stack_stats": ["arp_replies", "icmp_replies", "udp_echo", "discard_frames",
                    "discard_bytes", "discard_bytes_hi", "peer_ip", "peer_mac_hi",
                    "peer_mac_lo", "blast_sent", "blast_busy", "not_ours"],
    "tcp_stats": ["started", "step", "step_error", "accepts", "active", "refused",
                  "sent_lo", "sent_hi", "recv_lo", "recv_hi", "send_errors",
                  "recv_errors", "last_send_error", "last_recv_error", "sndbuf_result",
                  "rcvbuf_result"],
    "xnet_stats": ["enabled", "patched", "user", "xmit_usb", "xmit_fallback",
                   "rx_delivered", "rx_filtered", "rx_no_user", "link_polls",
                   "flags_before", "flags_after", "link_notifies", "xmit_dropped",
                   "xq_high", "link_fallback"],
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


def read_struct(sym, name, index=0):
    """One struct by symbol; `index` picks an element of an array of them (the
    per-device net_stats, cdc_stats, rndis_stats, rtl_stats)."""
    fields = STRUCTS[name]
    raw = read(sym[name] + index * 4 * len(fields), 4 * len(fields))
    return dict(zip(fields, struct.unpack(">%dI" % len(fields), raw)))


NET_MAX = 4


def uplink_index(sym):
    """The device slot xnet is given, or None (the onboard port)."""
    a = struct.unpack(">I", read(sym["net_active"], 4))[0]
    return a - 1 if a else None


def uplink_stats(sym):
    """The uplink's net_stats (slot 0's when there is none)."""
    return read_struct(sym, "net_stats", uplink_index(sym) or 0)


def command(sym, what, *args, wait=30.0):
    """Arguments first, the request word last: the worker takes the request the
    moment it sees it, so it must not see it before the arguments are in place."""
    base = sym["xe_cmd"]
    # The symbols are build/'s: if the console runs another build, xe_cmd is some
    # other word of its memory. Its xe_state.magic must be there first.
    magic = struct.unpack(">I", read(sym["xe_state"], 4))[0]
    if magic != 0x58454452:
        raise SystemExit("xe_state.magic is %08x, not 'XEDR': the console runs another "
                         "build than build/ (make the same bench= first)" % magic)
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


def driver_names(sym):
    """net_stats.driver is an index into xe_drivers + 1; the names are the
    xe_driver_<name> symbols in table order, which is the order of DRIVERS."""
    table = sym.get("xe_drivers")
    named = {addr: name[len("xe_driver_"):] for name, addr in sym.items()
             if name.startswith("xe_driver_") and name != "xe_driver_count"}
    if table is None:
        return {}
    count = struct.unpack(">I", read(sym["xe_driver_count"], 4))[0]
    ptrs = struct.unpack(">%dI" % count, read(table, 4 * count))
    return {i + 1: named.get(p, "%08x" % p) for i, p in enumerate(ptrs)}


def status(sym):
    st = read_struct(sym, "xe_state")
    nst = read_struct(sym, "net_state")
    cf = read_struct(sym, "xe_config")
    ss = read_struct(sym, "stack_stats") if "stack_stats" in sym else None
    xs = read_struct(sym, "xnet_stats")
    mac = read(sym["net_mac"], 6)
    names = {v: k for k, v in MODES.items()}
    drivers = driver_names(sym)
    up = uplink_index(sym)
    print("plugin   magic=%08x version=%08x ticks=%d" % (
        st["magic"], st["version"], st["worker_ticks"]))
    print("config   mode=%s advertise=0x%02x ip=%s" % (
        names.get(cf["mode"], cf["mode"]), cf["advertise"], ip(cf["ip"])))
    print("running  mode=%s start=%d/%#x claims=%d/%d xnet=%#x uplink=%s mac=%s" % (
        names.get(nst["mode"], nst["mode"]), nst["start_count"], nst["start_result"],
        st["claim_found"], st["claim_attempts"], nst["xnet_status"],
        "slot %d" % up if up is not None else "onboard", mac.hex(":")))
    cm = struct.pack(">II", nst["console_mac_read0"], nst["console_mac_read1"])[:6]
    print("console  mac=%s from_config=%d status=%#x size=%d" % (
        cm.hex(":"), nst["console_mac_from_config"], nst["console_mac_status"],
        nst["console_mac_needed"]))
    for i in range(NET_MAX):
        ns = read_struct(sym, "net_stats", i)
        if not ns["driver"]:
            continue
        where = ns["where"]
        print("%s slot %d %-8s ready=%d link=%d speed=%dM ctrl=%d port=%d step=%d/%#x "
              "rx=%d tx=%d drops=%d not_uplink=%d" % (
                  "*" if i == up else " ", i, drivers.get(ns["driver"], ns["driver"]),
                  ns["ready"], ns["link"], ns["speed_mbps"], where & 3,
                  (where >> 2) & 7, ns["step"], ns["step_status"], ns["rx_frames"],
                  ns["tx_frames"], ns["tx_drops"], ns["rx_not_uplink"]))
    if "cdc_stats" in sym:
        for i in range(2):
            cs = read_struct(sym, "cdc_stats", i)
            if not cs["config_value"]:
                continue
            print("cdc %d    config=%d transport=%s intr=%d/%d parked=%d" % (
                i, cs["config_value"], "ncm" if cs["transport"] else "ecm",
                cs["intr_events"], cs["intr_errors"], cs["intr_parked"]))
    if "rndis_stats" in sym:
        for i in range(2):
            rs = read_struct(sym, "rndis_stats", i)
            if not rs["max_transfer"]:
                continue
            print("rndis %d  init=%#x max_pkts=%d max_xfer=%d align=%d media=%d "
                  "rx_xfers=%d bad=%d tx_xfers=%d" % (
                      i, rs["init_status"], rs["max_packets"], rs["max_transfer"],
                      rs["alignment"], rs["media_status"], rs["rx_transfers"],
                      rs["rx_bad"], rs["tx_transfers"]))
    if "aic_stats" in sym:
        for i in range(NET_MAX):
            a = read_struct(sym, "aic_stats", i)
            if not a["product"]:
                continue
            print("aic slot %d step=%d/%#x chip=%08x id=%d mcu=%d fw=%08x loaded=%d B in "
                  "%d files (missing %#x) cmds=%d timeouts=%d tx_err=%d/%#x rx=%d "
                  "err=%d/%#x msgs=%d unexpected=%d last=%#x data=%d" % (
                      i, a["step"], a["step_status"], a["chip_word"], a["chip_id"],
                      a["mcu_id"], a["fw_version"], a["bytes_loaded"], a["files_loaded"],
                      a["file_status"], a["commands"], a["timeouts"], a["tx_errors"],
                      a["tx_last_error"], a["rx_transfers"], a["rx_errors"],
                      a["rx_last_error"], a["rx_messages"], a["rx_unexpected"],
                      a["rx_last_id"], a["rx_data"]))
            print("aic rx0  " + " ".join("%08x" % a["rx_first%d" % k] for k in range(8)))
            text = b"".join(struct.pack(">I", a["fw_text%d" % k]) for k in range(12))
            if a["product"] == 0x8D81:
                print("aic fw   %r scan_results=%d" % (
                    text.split(b"\0")[0].decode("ascii", "replace"), a["scan_results"]))
                bssid = struct.pack(">II", a["bssid0"], a["bssid1"])[:6].hex(":")
                print("aic join connects=%d cfm=%#x result=%#x joined=%d "
                      "disconnects=%d/%#x refused_security=%d bssid=%s sta=%d aid=%d "
                      "freq=%d" % (
                          a["connects"], a["connect_status"], a["connect_result"],
                          a["joined"], a["disconnects"], a["disconnect_reason"],
                          a["join_refused_security"], bssid, a["ap_sta"], a["aid"],
                          a["join_freq"]))
                mac = struct.pack(">II", a["if_mac0"], a["if_mac1"])[:6].hex(":")
                print("aic if   vif=%d adds=%d/%#x removes=%d/%#x on %s | asked index=%d "
                      "freq=%d" % (a["vif"], a["if_adds"], a["if_add_status"],
                                   a["if_removes"], a["if_remove_status"], mac,
                                   a["join_index"], a["join_freq_asked"]))
                print("aic wpa  secured_joins=%d eapol in=%d out=%d dropped=%d "
                      "keys=%d key_status=%#x port=%#x refused=%d" % (
                    a["secured_joins"], a["eapol_in"], a["eapol_out"],
                    a["eapol_dropped"], a["keys_added"], a["key_status"],
                    a["port_status"], a["join_refused_security"]))
                print("aic auto scans=%d joins=%d missed=%d secured=%d | bt ep 0x83 "
                      "opened=%d status=%#x" % (
                    a["auto_scans"], a["auto_joins"], a["auto_missed"],
                    a["auto_secured"], a["bt_opened"], a["bt_open_status"]))
                print("aic data tx=%d full=%d err=%d/%#x | rx=%d frames=%d err=%d/%#x "
                      "not_data=%d unsupported=%d not_from_ds=%d short=%d trunc=%d ooo=%d | reorder held=%d late=%d timeouts=%d full=%d" % (
                          a["data_tx_frames"], a["data_tx_full"], a["data_tx_errors"],
                          a["data_tx_last_error"], a["data_rx_transfers"],
                          a["data_rx_frames"], a["data_rx_errors"],
                          a["data_rx_last_error"], a["data_rx_not_data"],
                          a["data_rx_unsupported"], a["data_rx_not_from_ds"],
                          a["data_rx_short"], a["data_rx_truncated"],
                          a["data_rx_out_of_order"], a["reorder_held"],
                          a["reorder_late"], a["reorder_timeouts"], a["reorder_full"]))
    if "wl_config" in sym:
        names_max = 4
        raw = read(sym["wl_config"], 8 + 4 * names_max + names_max * 33 + 4)
        loaded, count = struct.unpack(">II", raw[:8])
        lens = struct.unpack(">%dI" % names_max, raw[8:8 + 4 * names_max])
        base = 8 + 4 * names_max
        names = [raw[base + i * 33:base + i * 33 + lens[i]].decode("utf-8", "replace")
                 for i in range(min(count, names_max))]
        has_pass = struct.unpack(">I", raw[base + names_max * 33:
                                          base + names_max * 33 + 4])[0]
        print("wifi cfg loaded=%d ssid=%s pass=%s" % (
            loaded, ", ".join(repr(n) for n in names) or "none",
            "set" if has_pass else "none"))
    if "aic_bt_stats" in sym:
        b = read_struct(sym, "aic_bt_stats")
        addr = struct.pack(">II", b["address0"], b["address1"])[:6].hex(":")
        print("bt       up=%d addr=%s hci=%d.%d lmp=%d maker=%d | cmds=%d err=%d "
              "timeouts=%d events=%d/%#x reset=%#x ver=%#x addr_st=%#x" % (
                  b["up"], addr, b["hci_version"], b["hci_revision"],
                  b["lmp_version"], b["manufacturer"],
                  b["commands"], b["command_errors"], b["timeouts"], b["events"],
                  b["last_event"], b["reset_status"], b["version_status"],
                  b["address_status"]))
    if "wpa_stats" in sym:
        w = read_struct(sym, "wpa_stats")
        names = {0: "idle", 1: "wait-1", 2: "wait-3", 3: "done", 4: "failed"}
        print("wpa      state=%s psk=%d in=%d out=%d mic_bad=%d replays=%d "
              "unwrap_bad=%d no_gtk=%d fails=%d gtk=%dB key_info=%#x" % (
                  names.get(w["state"], w["state"]), w["psk_ready"],
                  w["messages_in"], w["messages_out"], w["mic_failures"],
                  w["replays"], w["unwrap_failures"], w["no_gtk"], w["failures"],
                  w["gtk_len"], w["last_key_info"]))
    if "wl_stats" in sym:
        w = read_struct(sym, "wl_stats")
        print("wireless scans=%d/%d results=%d bad=%d full=%d networks=%d status=%#x" % (
            w["scans"], w["scans_done"], w["results"], w["bad_frames"], w["table_full"],
            w["bss_count"], w["last_scan_status"]))
        raw = read(sym["wl_bss"], 52 * min(w["bss_count"], 24))
        for k in range(min(w["bss_count"], 24)):
            e = raw[52 * k:52 * (k + 1)]
            bssid = e[0:6].hex(":")
            ssid = e[8:8 + e[6]].decode("utf-8", "replace")
            rssi, sec = struct.unpack(">bB", e[41:43])
            freq, = struct.unpack(">H", e[44:46])
            print("  %s ch %3d %4d MHz %4d dBm sec %02x %r" % (bssid, e[7], freq, rssi,
                                                             sec, ssid))
    if "file_stats" in sym:
        f = read_struct(sym, "file_stats")
        base = sym["file_stats"] + 4 * len(STRUCTS["file_stats"])
        text = read(base, 192)
        pdir = text[:96].split(b"\0")[0].decode("ascii", "replace")
        last = text[96:].split(b"\0")[0].decode("ascii", "replace")
        print("files    opens=%d found=%d status=%#x read_err=%d/%#x dir=%r last=%r" % (
            f["opens"], f["found"], f["last_status"], f["read_errors"],
            f["last_read_status"], pdir, last))
    if "rtl_stats" in sym:
        for i in range(NET_MAX):
            rs = read_struct(sym, "rtl_stats", i)
            if rs["chip_version"]:
                print("rtl slot %d chip=%04x adv_status=%#x" % (
                    i, rs["chip_version"], rs["advertise_status"]))
    if ss:
        print("stack    arp=%d icmp=%d echo=%d discard=%d/%dB peer=%s blast=%d busy=%d" % (
            ss["arp_replies"], ss["icmp_replies"], ss["udp_echo"], ss["discard_frames"],
            ss["discard_bytes_hi"] << 32 | ss["discard_bytes"], ip(ss["peer_ip"]),
            ss["blast_sent"], ss["blast_busy"]))
    print("xnet     enabled=%d user=%08x xmit usb/fallback=%d/%d rx=%d filtered=%d "
          "polls=%d notifies=%d flags %#x->%#x" % (
              xs["enabled"], xs["user"], xs["xmit_usb"], xs["xmit_fallback"],
              xs["rx_delivered"], xs["rx_filtered"], xs["link_polls"],
              xs["link_notifies"], xs["flags_before"], xs["flags_after"]))
    print("xnet tx  queued_high=%d dropped=%d link_fallback=%d" % (
        xs["xq_high"], xs["xmit_dropped"], xs["link_fallback"]))
    try:
        ts = read_struct(sym, "tcp_stats")
    except KeyError:
        return
    print("tcp      started=%d step=%d/%#x accepts=%d active=%d refused=%d" % (
        ts["started"], ts["step"], ts["step_error"], ts["accepts"], ts["active"],
        ts["refused"]))
    print("tcp i/o  sent=%d recv=%d send_err=%d/%#x recv_err=%d/%#x" % (
        ts["sent_hi"] << 32 | ts["sent_lo"], ts["recv_hi"] << 32 | ts["recv_lo"],
        ts["send_errors"], ts["last_send_error"], ts["recv_errors"],
        ts["last_recv_error"]))
    print("tcp opts sndbuf_result=%#x rcvbuf_result=%#x" % (ts["sndbuf_result"],
                                                           ts["rcvbuf_result"]))


def prof(sym, seconds):
    """Two snapshots of net_prof, and what each stage cost between them."""
    if "net_prof" not in sym:
        print("this build has no profile; make bench=prof")
        return
    a = read_struct(sym, "net_prof")
    time.sleep(seconds)
    b = read_struct(sym, "net_prof")
    freq = b["freq"] or 50000000
    wall = ((b["stamp"] - a["stamp"]) & 0xFFFFFFFF) / freq
    print("wall %.3f s (console clock, %d Hz)" % (wall, freq))
    print("%-8s %9s %9s %10s %9s %7s" % ("stage", "calls/s", "us/call", "MB/s",
                                         "ns/byte", "busy%"))
    for one in PROF_SLOTS:
        get = lambda d, f: d[one + "_" + f + "_hi"] << 32 | d[one + "_" + f + "_lo"]
        ticks = get(b, "ticks") - get(a, "ticks")
        moved = get(b, "bytes") - get(a, "bytes")
        calls = (b[one + "_calls"] - a[one + "_calls"]) & 0xFFFFFFFF
        spent = ticks / freq
        print("%-8s %9.0f %9.2f %10.2f %9.2f %7.1f" % (
            one, calls / wall, spent / calls * 1e6 if calls else 0,
            moved / wall / 1e6, spent / moved * 1e9 if moved else 0,
            100 * spent / wall))


def history(sym, last):
    """The flight recorder (bench=history), oldest first, as rates per sample."""
    if "xe_history" not in sym:
        print("this build has no flight recorder; make bench=history")
        return
    names = ["tick", "rx_frames", "tx_frames", "tx_inflight", "tx_drops", "rx_errors",
             "xmit_usb", "xmit_dropped", "rx_bytes", "tx_bytes"]
    count = struct.unpack(">I", read(sym["xe_history_count"], 4))[0]
    size = 512
    raw = read(sym["xe_history"], size * 4 * len(names))
    rows = [dict(zip(names, struct.unpack_from(">%dI" % len(names), raw,
                                               i * 4 * len(names)))) for i in range(size)]
    have = min(count, size)
    order = [rows[(count - have + i) % size] for i in range(have)][-last:]
    print("%8s %7s %7s %7s %7s %5s %6s %6s %6s" % (
        "t(s)", "rx/s", "rxMB/s", "tx/s", "txMB/s", "infl", "drops", "rxerr", "xqdrop"))
    prev = None
    for r in order:
        if prev:
            dt = (r["tick"] - prev["tick"]) * 0.02 or 0.2
            d = lambda k: (r[k] - prev[k]) & 0xFFFFFFFF
            print("%8.1f %7.0f %7.2f %7.0f %7.2f %5d %6d %6d %6d" % (
                r["tick"] * 0.02, d("rx_frames") / dt, d("rx_bytes") / dt / 1e6,
                d("tx_frames") / dt, d("tx_bytes") / dt / 1e6, r["tx_inflight"],
                d("tx_drops"), d("rx_errors"), d("xmit_dropped")))
        prev = r


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
        # Any 32-bit global by name, e.g. `set cdc_tx_eager 4`.
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
    elif what == "history":
        history(sym, int(rest[0]) if rest else 512)
    elif what == "prof":
        prof(sym, float(rest[0]) if rest else 5.0)
    elif what == "cmd":
        print("result %#x status %#x" % command(sym, num(rest[0]),
                                                *[num(x) for x in rest[1:]]))
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
