# xedrivers

USB device drivers for the Xbox 360, delivered as one DashLaunch plugin: a shared
core, a layer for each kind of console interface, and a directory per device, of
which the build takes the ones named. Today that is wired networking -- the
Realtek RTL8156/RTL8156B USB 2.5G Ethernet adapter (`0bda:8156`), any CDC ECM/NCM
device, and Android phones tethering over RNDIS -- handed to the console's own
network stack in place of its onboard port. Built on Linux with nothing but
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

### The host controllers' own registers

Read with the plugin (`xectl.py cmd 16 <address> 1`; XBDM's getmem refuses device
registers) on both EHCI controllers, 0x7FEA3000 and 0x7FEA5000:

| register | EHCI 0 | EHCI 1 | means |
|---|---|---|---|
| HCSPARAMS | 0x00101404 | 0x00101505 | 4 and 5 ports, one USB 1.1 companion each |
| HCCPARAMS | 0x7070 | 0x7070 | **no asynchronous park mode** (bit 2 clear), 32-bit only |
| USBCMD | 0x00010031 | 0x00010021 | running, async schedule on, **interrupt threshold 1 microframe** |

So the two knobs an EHCI driver has for bulk throughput are already where they
can go: park mode -- several back-to-back packets per pass over a queue head --
is not implemented, and completions are already signalled every 125 us. With
the transmit queue full throughout (41-44 of 64 requests waiting) and the
processor 90% idle, what is left is the controller moving 512-byte packets at
the rate it can fetch them: about 6.6 packets a microframe out (36.9 ns a byte),
about 7.5 in, of the 13 USB 2.0 allows. That is the hardware.

### The same card on a PC's USB 2.0

The twin RTL8156B on the PC, behind a USB 2.0 hub (480 Mbit/s), cabled to the same
1 Gbit/s router, iperf3 to the PC's own onboard NIC from a separate network
namespace (2026-10-19):

| configuration (Linux driver) | TCP out / in | UDP out / in |
|---|---|---|
| 1, Realtek's own (r8152) | 38.6 / 39.6 MB/s | -- |
| 3, CDC ECM (cdc_ether) -- what the console runs | **36.7 / 38.7** | 37.7 / 40.2 |
| 2, CDC NCM (cdc_ncm) | 34.2 / 32.3 | -- |

So the card does 37-40 MB/s over USB 2.0 in exactly the mode the console uses,
against the console's 23.3 out / 30 in. The card and the CDC transport are not the
limit; the console's EHCI controller is (above).

### Where a frame's time goes

`make bench=prof` times each stage with the time base (`src/core/prof.h`) and
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

`src/bench/tcp/` serves two ports out of RAM on the console's own stack -- 5201 sends,
5202 reads and discards, a thread a connection -- with xam's socket calls imported
by ordinal (the kit packs xam as a second import library). It is built
only with `make bench=tcp` and started with `tests/xectl.py cmd 14`; a plain build has
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

### Writing to the console: what holds FTP uploads

Measured 2026-10-19, 256 MB from the PC's RAM to `Hdd1` with ftpdll
(`tests/ftpbench.sh`), and the same path with no disk (`tests/tcpbench.py`):

| transport | TCP, RAM to RAM: out / in | FTP to and from Hdd1: up / down |
|---|---|---|
| ECM (default) | 21.9 / **27.0** MB/s | **20.4-20.5** / 19.3 MB/s |
| NCM, 4 KB blocks | 22.0 / 23.4 | 18.8 / 19.5 |
| NCM, 8 KB blocks | 22.5 / 26.7 | 19.5 / 19.7 |
| NCM, 16 KB blocks | 22.9 / 24.4 | -- |

ECM stays the default: it is the fastest for writing to the console, which is
what installing games is. The upload is not held by the network. A capture of
one shows the console advertising a **zero TCP window for 23.8% of the
transfer**, in 256 stalls of about 12 ms: ftpdll serves a transfer from one thread
that receives, then writes to the disk, and receives nothing while it writes.
20.4 / (1 - 0.238) = 26.8 MB/s, the RAM-to-RAM figure. Raising that thread's
priority from 5 to anything up to 24 (`xectl.py cmd 15 <thread id> <prio>`) changes
nothing, and neither does handing receive to another processor
(`cdc_rx_cpu` 1, 2, 4, 6: 19.5-20.1 MB/s). A server that keeps receiving while it
writes -- one thread on the socket, one on the file, a buffer between -- is what
would reach the other 6 MB/s.

`cdc_rx_cpu` 1 under the plugin's own TCP server at full rate once stopped the
console's network until a power cycle; with FTP every value worked. It stays 0.

## Building

    make                                   the plugin for people: xnet, advertise 2500
    make bench=history,stack               ... with the tuning tools named
    make bench=all                         ... with every one
    make MODE=standalone IP=10.36.0.2 bench=stack   the built-in responder
    make MODE=standalone ADVERTISE=1000 IP=192.168.50.2 bench=stack
    make bench=tcp                         add the RAM-to-RAM TCP server
    make test                              host unit tests
    make push                              put it on the Usb: medium (Orange Pi)
    make DRIVERS=cdc                       only the drivers named (default: all)

## The bench

Everything that exists to measure and tune lives in `src/bench/<tool>/` and is
built in only when named: `make bench=history,stack`, `make bench=all`. A plain
`make` is the plugin for people, with none of it (image 0x10000 against 0x18000
with every tool, and none of their cycles):

| `src/bench/` | what | used by |
|---|---|---|
| `history/` | flight recorder: frames, bytes, in-flight, drops every 200 ms, ~100 s | `xectl.py history` |
| `stack/` | `MODE=standalone`: ARP/ICMP/UDP echo/discard and the UDP generator | `speedtest.py`, `phoneblast.py`, `xectl.py blast` |
| `debug/` | commands 15-17: thread priority by id, read/write any word (MMIO too) | `xectl.py cmd 15/16/17` |
| `prof/` | where the stage timers sum up (hooks in `src/core/prof.h`) | `xectl.py prof` |
| `tcp/` | the RAM-to-RAM TCP server, the plugin's only imports from xam | `tcpbench.py` |

The core reaches it only through `src/core/bench.h` -- an init, a tick, unknown
commands and the standalone mode -- which without any tool is empty inlines.
`MODE=standalone` needs `bench=stack`.

## Drivers

Everything that does not depend on the device is in `src/core/`: the USB driver
object, the worker and its command channel. The network layer is `src/net/`: the
network device that xnet and the standalone stack see, xnet itself, and who
consumes the frames; the core calls it through three entry points.
Each card driver is a directory of its own under `src/drivers/`, and the core
reaches it only through `src/core/driver.h`: one `XE_DRIVER` named
`xe_driver_<directory>` with `match`, `start`, `detached`, `xmit`, `tx_space` and
an optional `command`. The Makefile generates the table of drivers from the
directories, so adding a card is adding a directory and removing one is deleting
it. When the kernel offers a device every driver's `match` is asked; a driver that
knows the product (VID:PID) wins over one that only knows the class.

| driver | matches | what it is |
|---|---|---|
| `cdc` | class: a CDC ECM or NCM interface in the current configuration | the frame transport, ECM or NCM (`cdc.c` parses, `cdc_net.c` runs it) |
| `rtl8156` | `0bda:8156` | `cdc` plus the Realtek part: 2.5G advertisement, registers, PHY, chip version |
| `rndis` | class: an RNDIS control interface (E0/01/03, 02/02/FF, EF/04/01) | Remote NDIS, the way Android phones tether (`rndis.c` the messages, `rndis_net.c` the driver) |

`rndis` was first run on a Xiaomi phone tethering over USB (2717:ff80): INITIALIZE,
MAC, filter and link state answered at the first try, the phone packs several
received frames into one transfer (70 in 39), and in xnet mode the console took
an address from the phone's DHCP and served FTP to a client on the phone. The
console sends to a phone at 24.5 MB/s with several frames packed into one
transfer (21.2 one a transfer; tests/phoneblast.py, UDP to the phone's broadcast
address, rate as the phone's USB side accepted it). Offered 16 KB transfers, the phone held received frames back to fill them, and an
FTP upload from it crawled at 75-90 KB/s; offered 2048 bytes, as Linux
rndis_host does (`rndis_rx_max`), it sends one frame a transfer and an 80 MB and a
500 MB file went both ways quickly, 714k frames in and 642k out without an error.
The
phone enables tethering only once the console is up, which is fine: the kernel
offers it to the plugin when it arrives.

`cdc` alone does not take an RTL8156: the console's kernel offers a device in its
first configuration, which on this card is Realtek's own protocol, so no CDC
interface is ever shown to `match` (measured: `make DRIVERS=cdc` leaves the card
unclaimed). It is for devices that are CDC from the start.

A driver that builds on another includes its public header, which is the whole of
the dependency: `make DRIVERS=rtl8156` without `cdc` stops at that include. Tests
sit beside the code: `tests/core/`, `tests/drivers/<d>/`.

## When the card goes

Unplug the card -- or its cable, or end a phone's tethering -- and the console goes
on with its onboard port: without the card's link the link-state hook answers
with the EMAC's, transmit takes the kernel's EMAC path, and the EMAC delivers to
the title itself. Plug it back and it is offered to the plugin again, brought up,
and xnet moves back once its link is up. Measured: the onboard port answered at
once each time; back on the card 4 s after its PHY came up, and at once after a
physical replug of the card.

## The worker and the dimmed screen

About ten minutes after the last pad input the dashboard dims the screen, and from
then on a thread waiting on hardware thread 2 -- where ExCreateThread puts the
plugin's worker, and where the USB stack runs -- is not woken again until a pad is
touched: neither its timeout nor a set event brings it back. Frames kept flowing
(they are completions, not the worker), but a card plugged in meanwhile was not
brought up, and the notification endpoint was not re-armed. The worker now runs on
hardware thread 5, where it went on past the dim; it also waits on an event that a
device arriving or leaving and a finished control transfer set, instead of
sleeping in fixed steps. Why thread 2 sleeps through the dim is not known.

## The interrupt endpoint

The RTL8156 in ECM answers every poll of its notification endpoint with the same
CONNECTION_SPEED_CHANGE and NETWORK_CONNECTION, alternately -- under Linux
cdc_ether too, at the 128 ms its bInterval asks for. Here it came to 62.5 transfers
a second: UsbdOpenEndpoint takes the interval in milliseconds and polls no
less often than every 32 ms, and the high-speed bInterval (an exponent, 11) was
passed raw. Now it is converted (32 ms), and a notification identical to the last of
its kind parks the endpoint for about a second, while a change is acted on and
re-armed at once: 1.0 transfer a second, a pulled cable seen within 0.9 s. TCP was
21.9 MB/s out with 62.5, 31.2, 1 or no notifications a second -- it is the bus and
the USB stack that are spared, not the throughput.

## Modes

| `MODE` | frames go to |
|---|---|
| `xnet` (default) | the console's network stack, in place of the onboard EMAC |
| `standalone` | a built-in responder on `IP`: ARP, ping, UDP echo (7), discard (9), and a UDP generator for throughput runs. The console's own network is untouched. |
| `drop` | nowhere (bring the card up only) |

`ADVERTISE` is the fastest link offered: `2500`, `1000`, `100`, or `default` (the
card's firmware choice). `TRANSPORT` is `ecm` (default) or `ncm`. Both work and both stop at the
console's USB ceilings (~31 MB/s in, ~24 MB/s out); NCM is no faster and, with
16 KB blocks, slower for FTP uploads -- the measurements are in `src/drivers/cdc/cdc_net.c`.

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
