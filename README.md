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
| XBDM getfile / sendfile | 21.1 / 23.5 MB/s | |
| XBDM getmemex (console RAM) | 5 Mbit/s | 5 Mbit/s |
| UDP out, by payload 64 / 512 / 1000 B | 81k / 32k / 19.7k frames/s | |
| ping | 0.2 ms | |

XBDM's memory read is limited by XBDM itself (the same on both ports).

## Where the limit is

Sweeping the frame size while transmitting fits the console's outgoing USB path to
10.9 us a transfer plus **36.9 ns a byte** (78 to 1486 byte frames, 6.4 to 23.3
MB/s). The per-byte term dominates at any useful frame size, which is why carrying
ten frames in one transfer (NCM) moved nothing: the ceiling is about 27 MB/s for an
infinitely large frame and 23.3 MB/s at the Ethernet MTU. Inbound there is no such
wall below the bus: 30 MB/s measured.

Against those ceilings, both directions and three servers:

| console -> PC | MB/s | of ceiling | PC -> console | MB/s | of ceiling |
|---|---|---|---|---|---|
| ceiling (model) | 23.3 | | ceiling (measured) | 30.0 | |
| our UDP, no xnet | 23.8 | 100% | our UDP, no xnet | 30.0 | 100% |
| XBDM, xnet + disk | 21.1 | 91% | XBDM, xnet + disk | 23.5 | 78% |
| ftpdll, xnet + disk | 19.7 | 85% | ftpdll, xnet + disk | 20.5 | 68% |

So the driver already runs at the hardware ceiling, xnet and TCP cost little, and
what is left belongs to whatever serves the files: XBDM's own transfer beats ftpdll
by 7% outbound and 15% inbound over the same card. Outbound there is at most 18%
left to win; inbound, where the bus is not the wall, a better server has room to
roughly 26-28 MB/s -- which is what CUsbEnet's author reports for SMB/FTP/NFS with
his own server and tuned buffers, against 32 MB/s to a plain speed-test socket.

### Where a frame's time goes

`make PROF=1` times each stage with the time base (`src/prof.h`) and
`tests/xectl.py prof 5` reads it. During the TCP runs below, single stream:

| stage | console -> PC (15.0k frames/s) | PC -> console (18.5k frames/s) |
|---|---|---|
| copy into the transfer buffer | 5.7 us a frame (3.8 ns/B), 8.6% of a thread | -- |
| UsbdQueueAsyncTransfer | 0.8 us, 1.6% | 0.8 us, 2.1% |
| transmit completion | 0.7 us, 1.1% | 0.7 us, 0.7% |
| receive completion | (ACKs) 6.4% | 11.3 us, 21.0% |
| of it xnet's receive (TCP) | 5.7% | 10.1 us, 18.7% |

Outbound a frame leaves every 66.5 us, and of that the processor spends about 7 us
on it; meanwhile 41 to 44 of the 64 transmit requests are queued at the host
controller the whole time. The bus, not the processor, is what the frames wait
for: copying less (or not at all) would free 8.6% of a thread and move nothing.
Inbound, the USB thread is 21% busy, mostly in xnet's own TCP receive -- not the
limit either.

### RAM to RAM, the plugin's own TCP server

`src/tcp.c` serves two ports out of RAM on the console's own stack -- 5201 sends,
5202 reads and discards, a thread a connection -- with xam's socket calls imported
by ordinal (`tools/build_plugin.py` packs the second import library). It is built
only with `make TCP=1` and started with `tests/xectl.py cmd 14`; a plain build has
no xam imports at all, so the driver never depends on xam to load. Measured with
`tests/tcpbench.py`, router at 1 Gbit/s:

| | console -> PC | PC -> console |
|---|---|---|
| default thread priority | 4.5 MB/s | 2.5 MB/s |
| **priority 20** | **21.9 MB/s** (175 Mbit/s) | **27.0 MB/s** (216 Mbit/s) |
| 2 or 4 streams, priority 20 | 21.9 / 21.8 in total | 26.9 / 27.0 in total |

At the default priority a capture showed the sending thread idle 78% of the time,
in 13 ms gaps: it waits in `send()` for xnet to free buffer space and then for
another thread's quantum. Priorities 18 and 24 are as bad as the default, every
time -- most likely those of xnet's own threads -- while 19, 20, 22, 28 and 31 all
give the same 22 / 27. With it set, buffer sizes, chunk size and Nagle change
nothing: this is the path's ceiling, 94% of the 23.3 MB/s the USB allows outbound
and 90% of the 30 MB/s inbound. That is the "thread scheduling" CUsbEnet's author
named, and the gap to his 32 MB/s socket figure is now 5 MB/s, inbound.

## Building

    make                                   xnet, advertise 2500
    make MODE=standalone IP=10.36.0.2      the built-in responder, for measuring
    make MODE=standalone ADVERTISE=1000 IP=192.168.50.2
    make TCP=1                             add the RAM-to-RAM TCP server
    make test                              host unit tests
    make push                              put it on the Usb: medium (Orange Pi)

## Modes

| `MODE` | frames go to |
|---|---|
| `xnet` (default) | the console's network stack, in place of the onboard EMAC |
| `standalone` | a built-in responder on `IP`: ARP, ping, UDP echo (7), discard (9), and a UDP generator for throughput runs. The console's own network is untouched. |
| `drop` | nowhere (bring the card up only) |

`ADVERTISE` is the fastest link offered: `2500`, `1000`, `100`, or `default` (the
card's firmware choice). `TRANSPORT` is `ecm` (default) or `ncm`. Both work and both stop at the
console's USB ceilings (~31 MB/s in, ~24 MB/s out); NCM is no faster and, with
16 KB blocks, slower for FTP uploads -- the measurements are in `src/net.c`.

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
