#!/usr/bin/env python3
"""Switch the host's RTL8156 between its USB configurations, over usbfs.

For checking what the card does under Linux's own class drivers: detach whatever
kernel driver holds its interfaces, select a configuration, and let the kernel
probe the new interfaces (config 2 binds cdc_ncm, 3 cdc_ether). `reset` re-probes
the device, after which r8152's configuration selector puts it back on config 1.

    usbcfg.py 2        NCM under cdc_ncm
    usbcfg.py reset    back to r8152
    usbcfg.py connect  let the kernel probe interfaces usbfs detached
"""

import fcntl
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rtlreg import find_device  # noqa: E402

USBDEVFS_SETCONFIGURATION = 0x80045505      # _IOR('U', 5, unsigned int)
USBDEVFS_IOCTL = 0xC0105512                 # _IOWR('U', 18, struct usbdevfs_ioctl)
USBDEVFS_DISCONNECT = 0x5516                # _IO('U', 22)
USBDEVFS_RESET = 0x5514                     # _IO('U', 20)
USBDEVFS_CONNECT = 0x5517                   # _IO('U', 23)


def main(argv):
    fd = os.open(find_device(), os.O_RDWR)
    if argv[0] == "connect":
        # Hand interfaces usbfs detached back to the kernel to probe again.
        for ifno in range(4):
            try:
                fcntl.ioctl(fd, USBDEVFS_IOCTL, struct.pack("iixxxxQ", ifno,
                                                            USBDEVFS_CONNECT, 0))
                print("connected interface %d" % ifno)
            except OSError:
                pass
        return 0
    if argv[0] == "reset":
        fcntl.ioctl(fd, USBDEVFS_RESET)
        print("reset")
        return 0
    for ifno in range(4):
        try:
            fcntl.ioctl(fd, USBDEVFS_IOCTL, struct.pack("iixxxxQ", ifno,
                                                        USBDEVFS_DISCONNECT, 0))
            print("detached interface %d" % ifno)
        except OSError:
            pass
    fcntl.ioctl(fd, USBDEVFS_SETCONFIGURATION, struct.pack("I", int(argv[0])))
    print("configuration %s" % argv[0])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
