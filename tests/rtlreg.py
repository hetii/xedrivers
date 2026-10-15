#!/usr/bin/env python3
"""Read and write RTL8152/8153/8156 registers over usbfs, from the host.

The same vendor requests the plugin sends from the console (Linux r8152's
get_registers/set_registers): bmRequestType 0xC0/0x40, bRequest 0x05, wValue = the
register address, wIndex = MCU type (PLA 0x0100, USB 0x0000) | byte enables. The PHY
sits behind the OCP window: its 4 KB page goes into PLA 0xE86C, the register is read
at PLA 0xB000 | (addr & 0xFFF).

Works while the kernel's r8152 drives the card: vendor requests to the device
recipient need no claimed interface. Needs rw on the usbfs node (udev rule).

    rtlreg.py [--dev /dev/bus/usb/BBB/DDD] pla 0xe610        read a PLA dword
    rtlreg.py phy 0xa400                                      read a PHY register
    rtlreg.py phy 0xa5d4 0x0080                               write one
    rtlreg.py dump                                            the registers we care about
"""

import argparse
import ctypes
import fcntl
import glob
import os
import struct
import sys

USBDEVFS_CONTROL = 0xC0185500           # _IOWR('U', 0, struct usbdevfs_ctrltransfer)
PLA, USB = 0x0100, 0x0000
BYTE_EN_DWORD, BYTE_EN_WORD = 0xFF, 0x33
PLA_OCP_GPHY_BASE = 0xE86C


def find_device(vendor=0x0BDA, product=0x8156):
    for path in glob.glob("/sys/bus/usb/devices/*"):
        try:
            v = int(open(path + "/idVendor").read(), 16)
            p = int(open(path + "/idProduct").read(), 16)
        except OSError:
            continue
        if (v, p) == (vendor, product):
            bus = int(open(path + "/busnum").read())
            dev = int(open(path + "/devnum").read())
            return "/dev/bus/usb/%03d/%03d" % (bus, dev)
    raise SystemExit("no %04x:%04x on this host" % (vendor, product))


class Rtl:
    def __init__(self, node):
        self.fd = os.open(node, os.O_RDWR)
        # r8152 caches the OCP page it last selected; whatever we select is put
        # back in close(), or its next PHY access lands on our page.
        self.saved_base = self.read_word(PLA, PLA_OCP_GPHY_BASE)
        self.ocp_base = self.saved_base

    def close(self):
        if self.ocp_base != self.saved_base:
            self.write_word(PLA, PLA_OCP_GPHY_BASE, self.saved_base)
            self.ocp_base = self.saved_base

    def control(self, rtype, request, value, index, data_or_len):
        if isinstance(data_or_len, int):
            buf = ctypes.create_string_buffer(data_or_len)
            length = data_or_len
        else:
            buf = ctypes.create_string_buffer(bytes(data_or_len), len(data_or_len))
            length = len(data_or_len)
        req = struct.pack("=BBHHHIxxxxQ", rtype, request, value, index, length, 1000,
                          ctypes.addressof(buf))
        got = fcntl.ioctl(self.fd, USBDEVFS_CONTROL, bytearray(req), True)
        return buf.raw[:length]

    def read_dword(self, mcu, addr):
        raw = self.control(0xC0, 0x05, addr & ~3, mcu, 4)
        return struct.unpack("<I", raw)[0]

    def read_word(self, mcu, addr):
        shift = addr & 2
        raw = self.control(0xC0, 0x05, addr & ~3, mcu | (BYTE_EN_WORD << shift), 4)
        return (struct.unpack("<I", raw)[0] >> (shift * 8)) & 0xFFFF

    def write_word(self, mcu, addr, value):
        shift = addr & 2
        data = struct.pack("<I", (value & 0xFFFF) << (shift * 8))
        self.control(0x40, 0x05, addr & ~3, mcu | (BYTE_EN_WORD << shift), data)

    def phy_page(self, addr):
        base = addr & 0xF000
        if base != self.ocp_base:
            self.write_word(PLA, PLA_OCP_GPHY_BASE, base)
            self.ocp_base = base
        return (addr & 0x0FFF) | 0xB000

    def phy_read(self, addr):
        return self.read_word(PLA, self.phy_page(addr))

    def phy_write(self, addr, value):
        self.write_word(PLA, self.phy_page(addr), value)


INTERESTING = [
    ("PLA_TCR0 (version in hi word)", "pla", 0xE610),
    ("PLA_PHYSTATUS", "plaw", 0xE908),
    ("MII BMCR", "phy", 0xA400),
    ("MII BMSR", "phy", 0xA402),
    ("MII ADVERTISE", "phy", 0xA408),
    ("MII LPA", "phy", 0xA40A),
    ("MII CTRL1000", "phy", 0xA412),
    ("MII STAT1000", "phy", 0xA414),
    ("OCP_PHY_STATUS", "phy", 0xA420),
    ("OCP_10GBT_CTRL (bit7 = adv 2.5G)", "phy", 0xA5D4),
    ("OCP_10GBT_STAT (bit5 = lp 2.5G)", "phy", 0xA5D6),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dev")
    ap.add_argument("what", choices=["pla", "usb", "phy", "dump"])
    ap.add_argument("addr", nargs="?", type=lambda s: int(s, 0))
    ap.add_argument("value", nargs="?", type=lambda s: int(s, 0))
    a = ap.parse_args()
    r = Rtl(a.dev or find_device())
    try:
        run(r, a)
    finally:
        r.close()


def run(r, a):
    if a.what == "dump":
        for name, kind, addr in INTERESTING:
            if kind == "pla":
                v = r.read_dword(PLA, addr)
                print("%-34s %04x = %08x" % (name, addr, v))
            elif kind == "plaw":
                print("%-34s %04x = %04x" % (name, addr, r.read_word(PLA, addr)))
            else:
                print("%-34s %04x = %04x" % (name, addr, r.phy_read(addr)))
        return
    mcu = USB if a.what == "usb" else PLA
    if a.what == "phy":
        if a.value is None:
            print("%04x" % r.phy_read(a.addr))
        else:
            r.phy_write(a.addr, a.value)
    elif a.value is None:
        print("%08x" % r.read_dword(mcu, a.addr))
    else:
        r.write_word(mcu, a.addr, a.value)


if __name__ == "__main__":
    sys.exit(main())
