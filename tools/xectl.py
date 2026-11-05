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
    xectl.py omni                       what a wireless card registered by cmd 21 was asked
    xectl.py prof [SECONDS]             where a frame's time goes (bench=prof)
    xectl.py history [N]                the last N flight-recorder samples (200 ms)
    xectl.py bench tcp [up|down|both] [--streams 1,4] [--seconds S] [--pace MB/s]
    xectl.py bench ftp [up|down|both] [--mb 256] [--login U:P] [--dir Hdd1/xedrivers]
    xectl.py bench sockets [--count 80] how many TCP connections the console holds

Every bench result is set against the kept one under the same conditions and the
difference printed. With --record [--note TEXT] it is kept in docs/speedtest.jsonl,
in place of the old one, when better by more than
--tolerance (3%); within it nothing changes; worse by more is refused as a
possible regression unless --accept-lower REASON. --repeat N takes the median of
N runs. A record carries what the console runs, read back from it: card,
transport or channel, xnet settings, and the commit its plugin was built from
(none for a build of uncommitted changes). git log -p keeps the older figures.
tools/results.py turns those files into tables.

Environment: XE_HOST (the console's address, required), XERUNNER (the xerunner
CLI; default `xerunner` on PATH, else ../xerunner/.venv/bin/xerunner).
"""

import argparse
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ELF = os.path.join(ROOT, "build", "plugin.elf")
HOST = os.environ.get("XE_HOST", "")
XERUNNER = (os.environ.get("XERUNNER") or shutil.which("xerunner") or
            os.path.join(ROOT, "..", "xerunner", ".venv", "bin", "xerunner"))
RESULTS = os.path.join(ROOT, "docs", "speedtest.jsonl")

CMD = {"ping": 1, "reset": 2, "claim": 3, "blast": 4, "mode": 5, "restart": 6,
       "advertise": 7, "reg_read": 8, "reg_write": 9, "phy_read": 10, "phy_write": 11,
       "xnet_iface": 13, "tcp_start": 14}
MODES = {"drop": 0, "standalone": 1, "xnet": 2}
ADVERT = {"2500": 0x6F, "1000": 0x2F, "100": 0x0F, "default": 0}
MCU = {"pla": 0x0100, "usb": 0x0000}

PROF_SLOTS = ['copy', 'submit', 'tx_done', 'rx_done', 'rx_up', 'tx_wait', 'turnaround']

STRUCTS = {
    "usb_stats": ["match_calls", "add_calls", "remove_calls", "close_passes", "submit_direct",
                  "submit_dpc", "dpc_runs", "last_open_status", "scan_found", "reset_status",
                  "unbound_resets", "no_slot", "control_late", "cancels", "nak_reload_set",
                  "nak_reload_skipped"],
    "net_prof": ["enabled", "freq", "stamp", "copy_ticks_hi", "copy_ticks_lo", "copy_bytes_hi", "copy_bytes_lo", "copy_calls", "submit_ticks_hi", "submit_ticks_lo", "submit_bytes_hi", "submit_bytes_lo", "submit_calls", "tx_done_ticks_hi", "tx_done_ticks_lo", "tx_done_bytes_hi", "tx_done_bytes_lo", "tx_done_calls", "rx_done_ticks_hi", "rx_done_ticks_lo", "rx_done_bytes_hi", "rx_done_bytes_lo", "rx_done_calls", "rx_up_ticks_hi", "rx_up_ticks_lo", "rx_up_bytes_hi", "rx_up_bytes_lo", "rx_up_calls", "tx_wait_ticks_hi", "tx_wait_ticks_lo", "tx_wait_bytes_hi", "tx_wait_bytes_lo", "tx_wait_calls", "turnaround_ticks_hi", "turnaround_ticks_lo", "turnaround_bytes_hi", "turnaround_bytes_lo", "turnaround_calls"],
    "xe_state": ["magic", "version", "worker_ticks", "claim_attempts", "claim_found",
                 "build_commit", "build_dirty"],
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
    "ax178_stats": ["product", "eeprom_17", "phy_address", "phy_id", "bmsr", "lpa", "stat1000",
                    "medium", "link_checks", "rx_transfers", "rx_resyncs", "intr_events",
                    "intr_errors", "tx_transfers", "tx_too_big", "mii_errors"],
    "ax179_stats": ["bcd_device", "product", "eeprom_43", "phy_status", "medium", "usb_speed",
                    "link_resets", "rx_transfers", "rx_bad", "rx_bad_frames", "intr_events",
                    "intr_errors", "tx_transfers", "tx_too_big", "family_a", "chip_status",
                    "firmware", "intr_link"],
    "nfs_stats": ["started", "connections", "active", "calls", "bad_calls", "refused",
                  "reads", "read_bytes_lo", "read_bytes_hi", "writes", "write_bytes_lo",
                  "write_bytes_hi", "stale", "handles", "handles_full", "portmap_calls",
                  "mounts", "commits", "opens", "listings", "no_thread"],
    "ftp_stats": ["started", "sessions", "active", "refused", "bad_logins", "commands",
                  "retrs", "stors", "lists", "sent_lo", "sent_hi", "received_lo",
                  "received_hi", "data_failed", "wait_failed", "accept_failed",
                  "read_failed", "send_failed", "no_thread"],
    "rndis_stats": ["init_status", "max_packets", "max_transfer", "alignment",
                    "filter_status", "media_status", "link_speed", "control_polls",
                    "keepalives", "indications", "last_indication", "rx_transfers",
                    "rx_bad", "intr_events", "intr_errors", "tx_too_big",
                    "tx_transfers"],
    "rtw_stats": ["step", "step_status", "chip_version", "cut", "reg_reads", "reg_writes",
                  "reg_errors", "reg_last_error", "reg_last_address", "fw_size",
                  "fw_version", "fw_h2c_version", "fw_ctrl", "poll_timeouts",
                  "last_poll_address", "bulk_out", "bulk_errors", "bulk_last_error", "bulk_late",
                  "rfe_option", "crystal_cap", "mac_lo", "mac_hi", "out_ep0", "out_ep1",
                  "out_ep2", "out_ep3", "in_ep", "int_ep", "channel", "h2c_packets",
                  "rx_transfers", "rx_frames", "rx_bad", "rx_c2h", "rx_crc_errors",
                  "rx_errors", "rx_last_error", "rx_beacons", "rx_mgmt", "rx_data",
                  "last_signal", "scans", "tx_frames", "tx_done", "tx_errors",
                  "tx_last_error", "tx_drops", "tx_full", "keys", "linked", "h2c_commands",
                  "h2c_busy", "ra_mask", "ra_rate_id", "ampdu", "rx_largest",
                  "tx_inflight_now", "txpwr_ofdm6", "txpwr_vht2_mcs7", "iqk_runs", "iqk_ms",
                  "tx_stalls", "rx_stalls", "stall_inflight", "stall_ep_busy", "stall_open",
                  "tx_transfers", "tx_aggregated", "rxs_busy", "rxs_parked", "rxs_errors",
                  "rxs_last_error", "rxs_rxpkt_num", "rxs_rxdma_status", "rxs_txdma_status",
                  "rxs_fifo_info", "rx_kicks", "rx_kick_ok", "dig_igi", "dig_fa", "dig_rssi"],
    "rtw_sta_stats": ["state", "scans", "probes", "auths", "auth_status", "assocs",
                      "assoc_status", "aid", "joins", "timeouts", "deauths",
                      "deauth_reason", "beacon_losses", "rx_data", "rx_eapol",
                      "rx_dropped", "tx_data", "tx_dropped", "addba_requests",
                      "addba_accepted", "bw", "channel", "keys", "port_open", "rx_duplicates", "tx_ba",
                      "rx_ba", "r_held", "r_late", "r_jumps", "r_timeouts", "r_full",
                      "r_resyncs", "r_ended", "r_rescued"],
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
                  "bt_open_status", "bt_opened", "bssid0",
                  "bssid1", "data_tx_frames", "data_tx_full",
                  "data_tx_errors", "data_tx_last_error",
                  "data_rx_transfers", "data_rx_frames", "data_rx_errors",
                  "data_rx_last_error", "data_rx_not_data",
                  "data_rx_unsupported", "data_rx_not_from_ds",
                  "data_rx_short", "data_rx_truncated",
                  "data_rx_out_of_order", "reorder_held", "reorder_late", "reorder_jumps",
                  "reorder_timeouts", "reorder_full", "reorder_resyncs",
                  "reorder_ended", "reorder_rescued", "unexp0", "unexp1", "unexp2", "unexp3", "join_width", "join_center1",
                  "assoc_ht_cap", "assoc_vht_cap", "ap_vht_oper", "rx_kicks", "rx_kick_ok",
                  "chip_temp", "chip_temp_max", "tx_cap_kbps", "tx_capped", "tx_cap_lowest", "temp_failures"],
    "wpa_stats": ["state", "failures", "replays", "mic_failures", "messages_in",
                  "messages_out", "unwrap_failures", "last_key_info", "gtk_len",
                  "no_gtk", "pending", "applied", "psk_ready", "group_rekeys"],
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
                  "rcvbuf_result", "flight_samples", "flight_kb_sum", "flight_max",
                  "fh0", "fh1", "fh2", "fh3", "fh4", "fh5", "fh6", "fh7", "flight_resent",
                  "thread_failures", "thread_status"],
    "wl_bridge_stats": ["scans", "joins", "missed", "secured"],
    "wl_omni_stats": ["registered", "attached", "present", "link", "gets", "sets",
                      "unknown_gets", "unknown_sets", "last_unknown", "scans", "joins",
                      "missed", "leaves", "rejoins", "rx", "tx", "tx_dropped", "tx_high",
                      "security"],
    "debug_omni_log": ["seq", "op", "option", "length", "result", "repeats",
                       "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7"],
    "debug_tx_stall": ["stalls", "inflight", "tx_frames", "rx_frames", "tick"],
    "xnet_stats": ["enabled", "patched", "user", "xmit_usb", "xmit_fallback",
                   "rx_delivered", "rx_filtered", "rx_no_user", "link_polls",
                   "flags_before", "flags_after", "link_notifies", "xmit_dropped",
                   "xq_high", "link_fallback", "rcvbuf_before", "rcvbuf_after", "sndbuf_before", "sndbuf_after", "cwnd_kb", "cwnd_refused", "xq_stalls", "xq_stall_depth", "pool_added_kb", "pool_refused", "tcp_sockets_before", "tcp_sockets_after", "tcp_sockets_refused", "title_pool_pages", "title_pool_refused", "xq_head_drops", "xq_head_length", "xq_head_type"],
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
    if not HOST:
        raise SystemExit("set XE_HOST to the console's address")
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
    print("plugin   magic=%08x version=%08x ticks=%d built from %08x%s" % (
        st["magic"], st["version"], st["worker_ticks"], st["build_commit"],
        " with uncommitted changes" if st["build_dirty"] else ""))
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
    if "rtw_stats" in sym:
        for i in range(NET_MAX):
            r = read_struct(sym, "rtw_stats", i)
            if not r["reg_reads"]:
                continue
            mac = struct.pack(">IH", r["mac_lo"], r["mac_hi"]).hex(":")
            print("rtw slot %d step=%d/%#x chip=%08x cut=%d regs r=%d w=%d err=%d/%#x@%#x "
                  "fw=%dB v%d.%d.%d h2c=%d ctrl=%#06x polls_lost=%d@%#x bulk=%d err=%d/%#x" % (
                      i, r["step"], r["step_status"], r["chip_version"], r["cut"],
                      r["reg_reads"], r["reg_writes"], r["reg_errors"], r["reg_last_error"],
                      r["reg_last_address"], r["fw_size"], r["fw_version"] >> 16,
                      (r["fw_version"] >> 8) & 255, r["fw_version"] & 255,
                      r["fw_h2c_version"], r["fw_ctrl"], r["poll_timeouts"],
                      r["last_poll_address"], r["bulk_out"], r["bulk_errors"],
                      r["bulk_last_error"]))
            print("rtw efuse mac=%s rfe=%d xtal=%#x eps out=%02x/%02x/%02x in=%02x int=%02x" % (
                mac, r["rfe_option"], r["crystal_cap"], r["out_ep0"], r["out_ep1"],
                r["out_ep2"], r["in_ep"], r["int_ep"]))
            sig = r["last_signal"] - (1 << 32) if r["last_signal"] & 0x80000000 else r["last_signal"]
            print("rtw rx   ch=%d h2c=%d xfers=%d frames=%d bad=%d c2h=%d crc=%d err=%d/%#x "
                  "beacons=%d mgmt=%d data=%d signal=%d scans=%d" % (
                      r["channel"], r["h2c_packets"], r["rx_transfers"], r["rx_frames"],
                      r["rx_bad"], r["rx_c2h"], r["rx_crc_errors"], r["rx_errors"],
                      r["rx_last_error"], r["rx_beacons"], r["rx_mgmt"], r["rx_data"],
                      sig, r["scans"]))
            print("rtw tx   frames=%d done=%d err=%d/%#x drops=%d full=%d keys=%d linked=%d "
                  "h2c=%d busy=%d ra=%#x/%d ampdu=%d rx_largest=%d inflight=%d txpwr=%#x/%#x iqk=%d/%dms "
                  "transfers=%d aggregated=%d tx stalls=%d rx stalls=%d (inflight %d, low busy %d, open %d)" % (
                r["tx_frames"], r["tx_done"], r["tx_errors"], r["tx_last_error"],
                r["tx_drops"], r["tx_full"], r["keys"], r["linked"], r["h2c_commands"],
                r["h2c_busy"], r["ra_mask"], r["ra_rate_id"], r["ampdu"], r["rx_largest"],
                r["tx_inflight_now"], r["txpwr_ofdm6"], r["txpwr_vht2_mcs7"],
                r["iqk_runs"], r["iqk_ms"], r["tx_transfers"], r["tx_aggregated"],
                r["tx_stalls"], r["rx_stalls"], r["stall_inflight"], r["stall_ep_busy"], r["stall_open"]))
            print("rtw dig  igi=%#x false alarms=%d rssi=%d" % (r["dig_igi"], r["dig_fa"], r["dig_rssi"]))
            if r["rx_kicks"] or r["rx_stalls"]:
                print("rtw kick  receive taken back %d times, went on after %d" % (r["rx_kicks"], r["rx_kick_ok"]))
            if r["rx_stalls"]:
                print("rtw rxs  busy=%d parked=%d errors=%d/%#x rxpkt_num=%#010x rxdma=%#010x txdma=%#010x fifo=%#010x" % (
                    r["rxs_busy"], r["rxs_parked"], r["rxs_errors"], r["rxs_last_error"],
                    r["rxs_rxpkt_num"], r["rxs_rxdma_status"], r["rxs_txdma_status"], r["rxs_fifo_info"]))
            if "rtw_sta_stats" in sym:
                t = read_struct(sym, "rtw_sta_stats", i)
                states = ["idle", "scanning", "probing", "auth", "assoc", "joined"]
                print("sta      %s scans=%d probes=%d auth=%d/%d assoc=%d/%d aid=%d joins=%d "
                      "timeouts=%d deauths=%d/%d losses=%d ch=%d bw=%d keys=%d port=%d" % (
                          states[t["state"]] if t["state"] < 6 else t["state"], t["scans"],
                          t["probes"], t["auths"], t["auth_status"], t["assocs"],
                          t["assoc_status"], t["aid"], t["joins"], t["timeouts"],
                          t["deauths"], t["deauth_reason"], t["beacon_losses"], t["channel"],
                          t["bw"], t["keys"], t["port_open"]))
                print("sta data rx=%d dup=%d eapol=%d dropped=%d tx=%d dropped=%d | ba tx=%d rx=%#x "
                      "addba=%d/%d | reorder held=%d late=%d jumps=%d timeouts=%d full=%d "
                      "resyncs=%d rescued=%d" % (
                    t["rx_data"], t["rx_duplicates"], t["rx_eapol"], t["rx_dropped"],
                    t["tx_data"], t["tx_dropped"], t["tx_ba"], t["rx_ba"], t["addba_requests"],
                    t["addba_accepted"], t["r_held"], t["r_late"], t["r_jumps"],
                    t["r_timeouts"], t["r_full"], t["r_resyncs"], t["r_rescued"]))
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
                print("aic bt ep 0x83 opened=%d status=%#x" % (
                    a["bt_opened"], a["bt_open_status"]))
                print("aic data tx=%d full=%d err=%d/%#x | rx=%d frames=%d err=%d/%#x "
                      "not_data=%d unsupported=%d not_from_ds=%d short=%d trunc=%d ooo=%d | reorder held=%d late=%d jumps=%d timeouts=%d full=%d resyncs=%d ended=%d" % (
                          a["data_tx_frames"], a["data_tx_full"], a["data_tx_errors"],
                          a["data_tx_last_error"], a["data_rx_transfers"],
                          a["data_rx_frames"], a["data_rx_errors"],
                          a["data_rx_last_error"], a["data_rx_not_data"],
                          a["data_rx_unsupported"], a["data_rx_not_from_ds"],
                          a["data_rx_short"], a["data_rx_truncated"],
                          a["data_rx_out_of_order"], a["reorder_held"],
                          a["reorder_late"], a["reorder_jumps"], a["reorder_timeouts"], a["reorder_full"],
                          a["reorder_resyncs"], a["reorder_ended"]))
                print("aic unexpected messages, last four: %s" % " ".join(
                    "%#x" % a["unexp%d" % i] for i in range(4)))
                print("aic link width=%d center1=%d | our ht_cap=%#06x vht_cap=%#010x | "
                      "ap vht_oper=%#08x" % (a["join_width"], a["join_center1"],
                      a["assoc_ht_cap"], a["assoc_vht_cap"], a["ap_vht_oper"]))
            if a["rx_kicks"]:
                print("aic kick  receive taken back %d times, went on after %d" % (a["rx_kicks"], a["rx_kick_ok"]))
            print("aic temp  %d (most %d, %d unanswered) | transmit cap %s, lowest %d kbit/s, waited %d" % (
                a["chip_temp"], a["chip_temp_max"], a["temp_failures"],
                "%d kbit/s" % a["tx_cap_kbps"] if a["tx_cap_kbps"] else "off",
                a["tx_cap_lowest"], a["tx_capped"]))
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
              "unwrap_bad=%d no_gtk=%d fails=%d gtk=%dB key_info=%#x group_rekeys=%d" % (
                  names.get(w["state"], w["state"]), w["psk_ready"],
                  w["messages_in"], w["messages_out"], w["mic_failures"],
                  w["replays"], w["unwrap_failures"], w["no_gtk"], w["failures"],
                  w["gtk_len"], w["last_key_info"], w["group_rekeys"]))
    if "wl_bridge_stats" in sym:
        b = read_struct(sym, "wl_bridge_stats")
        print("bridge   auto scans=%d joins=%d missed=%d secured=%d" % (
            b["scans"], b["joins"], b["missed"], b["secured"]))
    if "wl_omni_stats" in sym:
        o = read_struct(sym, "wl_omni_stats")
        if o["registered"]:
            print("omni     registered=%#x attached=%d present=%d link=%d security=%d "
                  "gets=%d sets=%d unknown=%d/%d last=%#x" % (
                      o["registered"], o["attached"], o["present"], o["link"],
                      o["security"], o["gets"], o["sets"], o["unknown_gets"],
                      o["unknown_sets"], o["last_unknown"]))
            print("omni     scans=%d joins=%d missed=%d leaves=%d rejoins=%d rx=%d tx=%d "
                  "dropped=%d queue_high=%d" % (
                      o["scans"], o["joins"], o["missed"], o["leaves"], o["rejoins"],
                      o["rx"], o["tx"], o["tx_dropped"], o["tx_high"]))
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
    if "ax179_stats" in sym:
        for i in range(2):
            a = read_struct(sym, "ax179_stats", i)
            if a["product"]:
                print("ax179 %d  pid=%04x bcd=%04x %s eeprom43=%#x physr=%#06x medium=%#06x usb=%#x "
                      "link checks=%d intr link=%d rx=%d bad=%d/%d frames intr=%d err=%d tx=%d "
                      "too_big=%d" % (
                          i, a["product"], a["bcd_device"],
                          "AX88179A/B chip=%#04x fw=%06x" % (a["chip_status"], a["firmware"])
                          if a["family_a"] else "AX88179/178A",
                          a["eeprom_43"], a["phy_status"], a["medium"], a["usb_speed"],
                          a["link_resets"], a["intr_link"], a["rx_transfers"], a["rx_bad"],
                          a["rx_bad_frames"], a["intr_events"], a["intr_errors"],
                          a["tx_transfers"], a["tx_too_big"]))
    if "ax178_stats" in sym:
        for i in range(2):
            a = read_struct(sym, "ax178_stats", i)
            if a["product"]:
                print("ax178 %d  pid=%04x eeprom17=%#06x phy addr=%d id=%08x bmsr=%#06x lpa=%#06x "
                      "stat1000=%#06x medium=%#06x link checks=%d rx=%d resyncs=%d intr=%d err=%d "
                      "tx=%d too_big=%d mii_err=%d" % (
                          i, a["product"], a["eeprom_17"], a["phy_address"], a["phy_id"],
                          a["bmsr"], a["lpa"], a["stat1000"], a["medium"], a["link_checks"],
                          a["rx_transfers"], a["rx_resyncs"], a["intr_events"], a["intr_errors"],
                          a["tx_transfers"], a["tx_too_big"], a["mii_errors"]))
    if "nfs_stats" in sym:
        n = read_struct(sym, "nfs_stats")
        print("nfs      started=%d conns=%d active=%d calls=%d bad=%d refused=%d mounts=%d "
              "read=%d/%dMB write=%d/%dMB commits=%d opens=%d listings=%d handles=%d full=%d "
              "stale=%d portmap=%d no_thread=%d" % (
                  n["started"], n["connections"], n["active"], n["calls"], n["bad_calls"],
                  n["refused"], n["mounts"], n["reads"],
                  (n["read_bytes_hi"] << 32 | n["read_bytes_lo"]) >> 20, n["writes"],
                  (n["write_bytes_hi"] << 32 | n["write_bytes_lo"]) >> 20, n["commits"],
                  n["opens"], n["listings"], n["handles"], n["handles_full"], n["stale"],
                  n["portmap_calls"], n["no_thread"]))
    if "srv_send_retries" in sym:
        r = struct.unpack(">I", read(sym["srv_send_retries"], 4))[0]
        e = struct.unpack(">I", read(sym["srv_send_last_error"], 4))[0]
        tf = struct.unpack(">I", read(sym["srv_thread_failed"], 4))[0]
        tl = struct.unpack(">I", read(sym["srv_thread_last"], 4))[0]
        tr = struct.unpack(">I", read(sym["srv_thread_retries"], 4))[0]
        print("srv      send retries=%d last error=%d | thread starts retried=%d failed=%d "
              "last status=%#x" % (r, e, tr, tf, tl))
    if "ftp_stats" in sym:
        f = read_struct(sym, "ftp_stats")
        print("ftp      started=%d sessions=%d active=%d refused=%d bad_logins=%d commands=%d "
              "retr=%d/%dMB stor=%d/%dMB lists=%d data_failed=%d (wait %d accept %d) "
              "retr stopped by disk %d / send %d no_thread=%d" % (
                  f["started"], f["sessions"], f["active"], f["refused"], f["bad_logins"],
                  f["commands"], f["retrs"], (f["sent_hi"] << 32 | f["sent_lo"]) >> 20,
                  f["stors"], (f["received_hi"] << 32 | f["received_lo"]) >> 20, f["lists"],
                  f["data_failed"], f["wait_failed"], f["accept_failed"],
                  f["read_failed"], f["send_failed"], f["no_thread"]))
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
    print("xnet buf system sockets: receive %d KB -> %d KB, send %d KB -> %d KB" % (
        xs["rcvbuf_before"], xs["rcvbuf_after"], xs["sndbuf_before"], xs["sndbuf_after"]))
    u = read_struct(sym, "usb_stats")
    print("usb      match=%d add=%d remove=%d close passes=%d cancels=%d resets of unbound=%d "
          "no slot=%d control late=%d nak reload=%d/%d skipped" % (
              u["match_calls"], u["add_calls"], u["remove_calls"], u["close_passes"],
              u["cancels"], u["unbound_resets"], u["no_slot"], u["control_late"],
              u["nak_reload_set"], u["nak_reload_skipped"]))
    print("xnet q   stalls=%d depth at the last=%d head drops=%d (last %d B, type %#06x) | cwnd clamp %s" % (
        xs["xq_stalls"], xs["xq_stall_depth"], xs["xq_head_drops"], xs["xq_head_length"],
        xs["xq_head_type"],
        "refused (other xam)" if xs["cwnd_refused"] else
        ("%d KB" % xs["cwnd_kb"]) if xs["cwnd_kb"] else "stock 64 KB"))
    print("xnet mem system pool +%d KB%s | TCP sockets %d -> %s%s" % (xs["pool_added_kb"],
        " (refused %d)" % xs["pool_refused"] if xs["pool_refused"] else "",
        xs["tcp_sockets_before"], xs["tcp_sockets_after"] or "left",
        " (refused %d)" % xs["tcp_sockets_refused"] if xs["tcp_sockets_refused"] else ""))
    print("xnet mem titles' pool %d KB by default%s" % (xs["title_pool_pages"] * 4,
        " (refused %d)" % xs["title_pool_refused"] if xs["title_pool_refused"] else ""))
    if "debug_tx_stall" in sym:
        for i in range(NET_MAX):
            d = read_struct(sym, "debug_tx_stall", i)
            if d["stalls"]:
                print("tx stall slot %d: %d time(s), last at tick %d with %d outstanding, "
                      "tx=%d rx=%d" % (i, d["stalls"], d["tick"], d["inflight"],
                                       d["tx_frames"], d["rx_frames"]))
    try:
        ts = read_struct(sym, "tcp_stats")
    except KeyError:
        return
    print("tcp      started=%d step=%d/%#x accepts=%d active=%d refused=%d no_thread=%d/%#x" % (
        ts["started"], ts["step"], ts["step_error"], ts["accepts"], ts["active"],
        ts["refused"], ts["thread_failures"], ts["thread_status"]))
    print("tcp i/o  sent=%d recv=%d send_err=%d/%#x recv_err=%d/%#x" % (
        ts["sent_hi"] << 32 | ts["sent_lo"], ts["recv_hi"] << 32 | ts["recv_lo"],
        ts["send_errors"], ts["last_send_error"], ts["recv_errors"],
        ts["last_recv_error"]))
    print("tcp opts sndbuf_result=%#x rcvbuf_result=%#x" % (ts["sndbuf_result"],
                                                           ts["rcvbuf_result"]))
    if "flight_samples" in ts and ts["flight_samples"]:
        print("tcp flight on 5201: avg %d KB, max %d B, resent %d, by 16 KB: %s" % (
            ts["flight_kb_sum"] // ts["flight_samples"], ts["flight_max"], ts["flight_resent"],
            " ".join(str(ts["fh%d" % k]) for k in range(8))))


def omni_log(sym):
    """What a wireless card registered by cmd 21 was asked, oldest first."""
    if "debug_omni_log" not in sym:
        print("this build has no probe; make bench=debug")
        return
    state = struct.unpack(">I", read(sym["debug_omni_state"], 4))[0]
    calls = struct.unpack(">I", read(sym["debug_omni_calls"], 4))[0]
    print("probe state=%#x calls=%d" % (state, calls))
    raw = read(sym["debug_omni_log"], 104 * 128)
    rows = []
    for k in range(128):
        e = raw[104 * k:104 * (k + 1)]
        seq, op, opt, length, res, rep = struct.unpack(">6I", e[:24])
        if seq:
            rows.append((seq, op, opt, length, res, rep, e[24:104]))
    for seq, op, opt, length, res, rep, data in sorted(rows):
        print("%5d x%-5d %-4s %#010x (%5d) len=%-4d -> %#010x  %s" % (seq, rep + 1,
            ("get", "set", "link", "attach", "detach", "final")[op] if op < 6 else op, opt,
            opt if opt < 0x10000 else 0, length, res, data[:min(80, max(length, 0))].hex()))


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


# --- benches -------------------------------------------------------------------
#
# TCP against the plugin's own server (src/bench/tcp, `make bench=tcp`, started with
# `cmd 14`), RAM to RAM: down = console -> PC from port 5201, up = PC -> console
# into port 5202. Every stream runs in one window, the same for all of them,
# TCP_SETTLE after they start (connection setup, slow start, buffers filling), so
# a stream that connected late is not counted over a window of its own. Down is
# what this side received in it; up is what the console's server received
# (tcp_stats.recv, read at both ends of the window), not what went into this
# side's socket buffers. --pace caps each stream this side sends (Linux
# SO_MAX_PACING_RATE). Checked on 2026-11-02 over the RTL8156B, 1, 4 and 8
# streams each way: this, the PC kernel's own TCP counters (ss -ti bytes_acked,
# bytes_received) and the console server's counter agree within 0.1%. (The PC
# NIC's counters are no reference: igb updates them every ~2 s.)

TCP_SETTLE = 2.0
SO_MAX_PACING_RATE = 47


def tcp_stream(port, begin, end, out, index, up, pace):
    try:
        s = socket.create_connection((HOST, port), timeout=10)
    except OSError:
        out[index] = -1                     # never connected
        return
    if pace:
        s.setsockopt(socket.SOL_SOCKET, SO_MAX_PACING_RATE, int(pace * 1e6))
    s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF if up else socket.SO_RCVBUF, 4 << 20)
    block = bytes(256 << 10) if up else None
    counted = 0
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
    except OSError:
        pass
    finally:
        s.close()
    out[index] = counted


def console_received(sym):
    """The console server's received bytes, and when (the middle of the read)."""
    before = time.time()
    t = read_struct(sym, "tcp_stats")
    return (t["recv_hi"] << 32 | t["recv_lo"]), (before + time.time()) / 2


def tcp_run(streams, seconds, up, pace=0.0, sym=None):
    """Bytes per second moved by `streams` connections over one common window."""
    out = [0] * streams
    begin = time.time() + TCP_SETTLE
    end = begin + seconds
    threads = [threading.Thread(target=tcp_stream,
                                args=(5202 if up else 5201, begin, end, out, i, up, pace))
               for i in range(streams)]
    for t in threads:
        t.start()
    first = last = None
    if up and sym is not None:
        time.sleep(max(0.0, begin - time.time()))
        first = console_received(sym)
        time.sleep(max(0.0, end - time.time()))
        last = console_received(sym)
    for t in threads:
        t.join()
    failed = out.count(-1)
    if failed:
        print("     %d of %d streams never connected" % (failed, streams))
    if first and last:
        if first[1] > begin + 1 or last[1] > end + 1:
            print("     the console's counter was read late (%.1f s, %.1f s): not a steady "
                  "window, do not trust this run" % (first[1] - begin, last[1] - end))
        return (last[0] - first[0]) / (last[1] - first[1])
    return sum(n for n in out if n > 0) / seconds


def ftp_run(up, mb, login, folder):
    """FTP to or from the console's disk with curl, what installing a game does;
    curl's average over the whole transfer, the disk included. Bytes per second."""
    name = "xectl-%d.bin" % mb
    url = "ftp://%s@%s/%s/%s" % (login, HOST, folder, name)
    if up:
        src = os.path.join("/dev/shm" if os.path.isdir("/dev/shm") else "/tmp", name)
        if not os.path.exists(src) or os.path.getsize(src) != mb << 20:
            with open(src, "wb") as f:
                f.write(os.urandom(mb << 20))
        args = ["-T", src, "--ftp-create-dirs", "-w", "%{speed_upload}"]
    else:
        args = ["-o", os.devnull, "-w", "%{speed_download}"]
    done = subprocess.run(["curl", "-s", "--max-time", "900", *args, url],
                          capture_output=True, text=True)
    if done.returncode:
        raise SystemExit("curl failed (%d): %s" % (done.returncode, url))
    return float(done.stdout.strip() or 0)


def sockets_run(count):
    """Open up to `count` connections to the TCP server's sink, each sending a
    little, hold them, then check each still takes data. (opened, still sending)"""
    conns, failed = [], 0
    for _ in range(count):
        s = socket.socket()
        s.settimeout(4)
        try:
            s.connect((HOST, 5202))
            s.sendall(b"x" * 1000)
            conns.append(s)
        except OSError:
            s.close()
            failed += 1
            if failed >= 3:
                break
    time.sleep(3)
    alive = 0
    for s in conns:
        try:
            s.sendall(b"y" * 4000)
            alive += 1
        except OSError:
            pass
    for s in conns:
        s.close()
    return len(conns), alive


BW_MHZ = {0: 20, 1: 40, 2: 80, 3: 160}


def context(sym):
    """What the console runs, for a result record: the uplink's card and how it is
    attached, and the xnet settings in force."""
    drivers = driver_names(sym)
    slot = uplink_index(sym)
    ns = read_struct(sym, "net_stats", slot or 0)
    card = drivers.get(ns["driver"], "onboard") if slot is not None else "onboard"
    ctx = {"card": card, "kind": "wired", "link_mbps": ns["speed_mbps"]}
    if card in ("cdc", "rtl8156", "rtl8153") and "cdc_stats" in sym:
        # rtl8156 and rtl8153 run on cdc's transport and have its cdc_stats slot too.
        ctx["transport"] = "ncm" if read_struct(sym, "cdc_stats", slot)["transport"] else "ecm"
    elif card == "rtw8822b" and "rtw_sta_stats" in sym:
        t = read_struct(sym, "rtw_sta_stats", slot)
        ctx.update(kind="wifi", channel=t["channel"], width_mhz=BW_MHZ.get(t["bw"], t["bw"]))
    elif card == "aic8800" and "aic_stats" in sym:
        a = read_struct(sym, "aic_stats", slot)
        freq = a["join_freq"]
        ch = (freq - 2407) // 5 if freq < 3000 else (freq - 5000) // 5
        ctx.update(kind="wifi", channel=ch, width_mhz=BW_MHZ.get(a["join_width"], a["join_width"]))
    if ctx["kind"] == "wifi":
        ctx["band_ghz"] = 2.4 if ctx["channel"] <= 14 else 5
    xs = read_struct(sym, "xnet_stats")
    ctx["xnet"] = {
        "rcvbuf_kb": xs["rcvbuf_after"] or xs["rcvbuf_before"],
        "sndbuf_kb": xs["sndbuf_after"] or xs["sndbuf_before"],
        "cwnd_kb": xs["cwnd_kb"] or 64,
        "pool_extra_kb": xs["pool_added_kb"],
        "tcp_sockets": xs["tcp_sockets_after"] or xs["tcp_sockets_before"],
        "title_pool_kb": xs["title_pool_pages"] * 4,
    }
    return ctx


def built_from(sym):
    """The commit the running plugin was built from, read off the console, and
    whether its tree had uncommitted changes."""
    st = read_struct(sym, "xe_state")
    return "%08x" % st["build_commit"], bool(st["build_dirty"] or not st["build_commit"])


CONDITIONS = ("card", "kind", "link_mbps", "transport", "band_ghz", "channel", "width_mhz",
              "xnet", "test", "streams", "direction")


def kept_rows(kind):
    path = RESULTS                  # wired and Wi-Fi rows together; "kind" tells them apart
    if not os.path.exists(path):
        return path, []
    with open(path) as f:
        return path, [json.loads(line) for line in f if line.strip()]


def keep(ctx, test, streams, direction, bytes_per_s, a):
    """Set one result against the kept one under the same conditions, say how far
    apart they are, and with --record keep it: in place of the old when better
    by more than the tolerance, never when within it (noise), and when worse by
    more only with --accept-lower REASON -- a drop is a regression until shown
    otherwise. The file holds the current figure for each set of conditions; git
    log -p the older ones."""
    row = {"date": time.strftime("%Y-%m-%d"), "commit": ctx["commit"],
           **{k: v for k, v in ctx.items() if k not in ("commit", "dirty")},
           "test": test, "streams": streams, "direction": direction,
           "mbit_s": round(bytes_per_s * 8 / 1e6, 1), "mbyte_s": round(bytes_per_s / 1e6, 2)}
    key = [row.get(k) for k in CONDITIONS]
    path, rows = kept_rows(ctx["kind"])
    old = next((r for r in rows if [r.get(k) for k in CONDITIONS] == key), None)
    if old:
        change = (row["mbit_s"] - old["mbit_s"]) / old["mbit_s"] * 100 if old["mbit_s"] else 0.0
        verdict = ("within" if abs(change) <= a.tolerance else
                   "better" if change > 0 else "WORSE")
        print("     on file %.0f Mbit/s (%s, %s): %+.1f%%, %s +-%.0f%%" % (
            old["mbit_s"], old["date"], old["commit"], change,
            verdict, a.tolerance))
    else:
        verdict = "new"
    if not a.record:
        return
    if ctx["dirty"]:
        raise SystemExit("not kept: the plugin on the console was built from uncommitted "
                         "changes (or by hand); commit, make, push, boot, then --record")
    if verdict == "within":
        return
    if verdict == "WORSE":
        if not a.accept_lower:
            print("     not kept: possibly a regression (was %.0f at %s, now %.0f at %s); "
                  "--repeat to be sure, or --accept-lower REASON" % (
                      old["mbit_s"], old["commit"], row["mbit_s"], row["commit"]))
            return
        row["note"] = "lower accepted: " + a.accept_lower
    elif a.note:
        row["note"] = a.note
    rows = [r for r in rows if [r.get(k) for k in CONDITIONS] != key] + [row]
    os.makedirs(os.path.dirname(RESULTS), exist_ok=True)
    with open(path, "w") as f:
        f.writelines(json.dumps(r, ensure_ascii=False) + "\n" for r in rows)
    print("     kept in %s" % os.path.relpath(path, ROOT))


def median(values):
    v = sorted(values)
    return v[len(v) // 2] if len(v) % 2 else (v[len(v) // 2 - 1] + v[len(v) // 2]) / 2


DIRECTION = {"down": "console->pc", "up": "pc->console"}


def bench(sym, argv):
    ap = argparse.ArgumentParser(prog="xectl.py bench")
    ap.add_argument("test", choices=["tcp", "ftp", "sockets"])
    ap.add_argument("which", nargs="?", default="both", choices=["down", "up", "both"])
    ap.add_argument("--streams", default="1,4")
    ap.add_argument("--seconds", type=float, default=10)
    ap.add_argument("--pace", type=float, default=0, help="MB/s per stream sent")
    ap.add_argument("--mb", type=int, default=256)
    ap.add_argument("--login", default=os.environ.get("XE_FTP", "xboxftp:xboxftp"))
    ap.add_argument("--dir", default="Hdd1/xedrivers")
    ap.add_argument("--count", type=int, default=80)
    ap.add_argument("--repeat", type=int, default=1, help="runs; the median counts")
    ap.add_argument("--tolerance", type=float, default=3.0, help="percent taken as noise")
    ap.add_argument("--record", action="store_true", help="keep the results")
    ap.add_argument("--accept-lower", default="", metavar="REASON",
                    help="keep a result worse than the kept one, saying why")
    ap.add_argument("--note", default="")
    a = ap.parse_args(argv)
    if not HOST:
        raise SystemExit("set XE_HOST to the console's address")
    ways = ["down", "up"] if a.which == "both" else [a.which]
    if a.test == "sockets":
        opened, alive = sockets_run(a.count)
        print("sockets  %d connections held (asked %d), %d still taking data" % (
            opened, a.count, alive))
        return
    commit, dirty = built_from(sym)
    ctx = dict(context(sym), commit=commit, dirty=dirty)
    if a.test == "ftp":
        for w in sorted(ways, key=lambda w: w != "up"):     # the file goes up first
            runs = [ftp_run(w == "up", a.mb, a.login, a.dir) for _ in range(a.repeat)]
            rate = median(runs)
            print("ftp  %-4s %s  %d MB  %6.1f Mbit/s (%5.1f MB/s)%s" % (
                w, DIRECTION[w], a.mb, rate * 8 / 1e6, rate / 1e6,
                "  median of %d" % a.repeat if a.repeat > 1 else ""))
            keep(ctx, "ftp %d MB" % a.mb, 1, DIRECTION[w], rate, a)
        return
    for n in (int(x) for x in a.streams.split(",")):
        for w in ways:
            runs = [tcp_run(n, a.seconds, w == "up", a.pace if w == "up" else 0, sym)
                    for _ in range(a.repeat)]
            rate = median(runs)
            print("tcp  %-4s x%-3d %s  %6.1f Mbit/s (%5.1f MB/s)%s" % (
                w, n, DIRECTION[w], rate * 8 / 1e6, rate / 1e6,
                "  median of %d" % a.repeat if a.repeat > 1 else ""))
            keep(ctx, "tcp ram", n, DIRECTION[w], rate, a)


def main(argv):
    if not argv:
        print(__doc__)
        return 1
    sym = symbols()
    what, rest = argv[0], argv[1:]
    if what == "bench":
        bench(sym, rest)
        return 0
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
    elif what == "omni":
        omni_log(sym)
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
