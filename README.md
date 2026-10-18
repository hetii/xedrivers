# xedrivers

A driver for the Realtek RTL8156/RTL8156B USB 2.5G Ethernet adapter (`0bda:8156`) on
the Xbox 360, delivered as a DashLaunch plugin. Built on Linux with nothing but
`gcc-powerpc-linux-gnu` and `python3`: the ordinals and the packer are in `kit/`,
copied from xerunner.

## What it does

The card is switched into its CDC-ECM configuration, where its own firmware runs the
MAC, and its PHY is then told over Realtek vendor requests to advertise up to
2.5GBASE-T -- in ECM the firmware alone only offers 10/100. Frames move one per bulk
transfer, 32 receive transfers always queued, 64 transmit slots. In `xnet` mode (the
default) the console's own network stack uses the card instead of its onboard port:
DHCP, XBDM, FTP, titles.

## Measured

Console card on a 1 Gbit/s home router, the PC on the same router at 1 Gbit/s; the
card is on the console's USB 2.0 port (`tests/speedtest.py`, `tests/ramxfer.py`, curl).

| what | RTL8156 (this driver) | onboard port (100M) |
|---|---|---|
| UDP PC -> console | 242 Mbit/s (the USB 2.0 ceiling) | |
| UDP console -> PC | 188 Mbit/s | |
| FTP download, 1 session | 19.6 MB/s = 157 Mbit/s | 11.4 MB/s |
| FTP download, 2 / 4 sessions | 155 / 154 Mbit/s in total | |
| FTP upload (to the hard disk) | 20.5 MB/s = 164 Mbit/s | 10.7 MB/s |
| XBDM getmemex (console RAM) | 5 Mbit/s | 5 Mbit/s |
| ping | 0.2 ms | |

XBDM's memory read is limited by XBDM itself (the same on both ports). Several FTP
sessions share what one gets: the limit is the path, not a single TCP stream.
The link negotiates 2500 Mbit/s against another RTL8156 and 1000 against the router.

## Building

    make                                   xnet, advertise 2500
    make MODE=standalone IP=10.36.0.2      the built-in responder, for measuring
    make MODE=standalone ADVERTISE=1000 IP=192.168.50.2
    make test                              host unit tests
    make push                              put it on the Usb: medium (Orange Pi)

## Modes

| `MODE` | frames go to |
|---|---|
| `xnet` (default) | the console's network stack, in place of the onboard EMAC |
| `standalone` | a built-in responder on `IP`: ARP, ping, UDP echo (7), discard (9), and a UDP generator for throughput runs. The console's own network is untouched. |
| `drop` | nowhere (bring the card up only) |

`ADVERTISE` is the fastest link offered: `2500`, `1000`, `100`, or `default` (the
card's firmware choice). `TRANSPORT` is `ecm` (default) or `ncm`; NCM receives but
its transmit does not work on this card's firmware -- see `src/net.c`.

Both can be changed on a running console, no rebuild or reboot:

    tests/xectl.py status
    tests/xectl.py mode xnet
    tests/xectl.py advertise 1000
    tests/xectl.py phy 0xa400            read a PHY register (OCP address)
    tests/xectl.py blast 100000          UDP generator: 100000 datagrams to the peer

`tests/rtlreg.py` reads the same registers on the twin card under Linux, for
comparing what Linux r8152 programs with what the console has.

## Installing

Copy `build/xedrivers.xex` to the console and add it to `launch.ini`:

    plugin3 = Usb:\plugin\xedrivers.xex

Load untested builds from `Usb:` only: a plugin that hangs the boot from `Hdd:`
cannot be removed remotely.

## Credits

Modelled on [proferabg/CUsbEnet](https://github.com/proferabg/CUsbEnet) (GPL-2.0) and
Linux `r8152` (GPL-2.0) for the register interface; the kernel's USB driver interface
from the xerunner knowledge base.
