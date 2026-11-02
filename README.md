# xedrivers

USB device drivers for the Xbox 360, delivered as one DashLaunch plugin: a shared
core, a layer for each kind of console interface, and a directory per device, of
which the build takes the ones named. Today that is wired networking -- the
Realtek RTL8156/RTL8156B USB 2.5G Ethernet adapter (`0bda:8156`), any CDC ECM/NCM
device, and Android phones tethering over RNDIS -- handed to the console's own
network stack in place of its onboard port. Built on Linux with nothing but
`gcc-powerpc-linux-gnu` and `python3`: the ordinals and the packer are in `kit/`,
copied from xerunner.

## Cards and speeds

Every card measured the same way: the console on the home router (OpenWrt
ZBT-WE1326; Wi-Fi on 5 GHz VHT80, a metre away: channel 36 for the AIC8800, channel
116 for the RTL8822BU, which since 7a95aff sets its TX power from its efuse), the PC
wired to it at 1 Gbit/s. "TCP RAM" is the plugin's own server (`src/bench/tcp`,
`tools/xectl.py bench tcp`, 10 s a run), RAM to RAM with no disk; "FTP" is ftpdll, 64 MB to and from `Hdd1:`.
Each figure is the range of three or more runs; no `xnet_*` options. Rows 1-5 are
from before the xnet defaults of 2026-10-29 (xnet's own 16 KB receive buffer, see
below); rows 6-8 are of 2026-10-30, with them, on channel 116.

| # | card | VID:PID | test | console -> PC | PC -> console |
|---|---|---|---|---|---|
| 1 | Realtek RTL8156B, 2.5G Ethernet | `0bda:8156` | TCP RAM, 1-4 streams | 174–176 Mbit/s (21.8–22.0 MB/s) | 214–216 Mbit/s (26.8–27.0 MB/s) |
| 2 | Realtek RTL8156B, 2.5G Ethernet | `0bda:8156` | FTP 64 MB | 157–158 Mbit/s (19.6–19.7 MB/s) | 164 Mbit/s (20.5 MB/s) |
| 3 | AICSemi AIC8800D80, Wi-Fi 6 | `a69c:8d81` | TCP RAM, 1 stream | 103–104 Mbit/s (12.9–13.0 MB/s) | 62–66 Mbit/s (7.7–8.2 MB/s) |
| 4 | AICSemi AIC8800D80, Wi-Fi 6 | `a69c:8d81` | TCP RAM, 4 streams | 140–142 Mbit/s (17.5–17.8 MB/s) | 104–110 Mbit/s (13.0–13.7 MB/s) |
| 5 | AICSemi AIC8800D80, Wi-Fi 6 | `a69c:8d81` | FTP 64 MB | 86 Mbit/s (10.8 MB/s) | 90 Mbit/s (11.2 MB/s) |
| 6 | Realtek RTL8822BU, Wi-Fi 5 | `0bda:b812` | TCP RAM, 1 stream | 168 Mbit/s (21.0 MB/s) | 176–182 Mbit/s (22.0–22.8 MB/s) |
| 7 | Realtek RTL8822BU, Wi-Fi 5 | `0bda:b812` | TCP RAM, 4 streams | 159 Mbit/s (19.9 MB/s) | 206–210 Mbit/s (25.8–26.2 MB/s) |
| 8 | Realtek RTL8822BU, Wi-Fi 5 | `0bda:b812` | FTP 64 MB | 111–112 Mbit/s (13.9–14.0 MB/s) | 131–132 Mbit/s (16.4–16.5 MB/s) |

What a repeated run that came out lower was, so that none of it is taken for a
regression:

- **RTL8156B at 156 / 187 Mbit/s (19.5 / 23.4 MB/s), or 146 / 173 Mbit/s (18.3 /
  21.6 MB/s)**: an AIC8800 was plugged into the
  same USB host controller and shared its bandwidth. With the RTL8156B alone the
  same build is back at rows 1-2.
- **AIC8800D80 four streams out of the console at 0**: the card drops off the
  router after minutes of sustained sending ("disconnected due to excessive
  missing ACKs"), whatever card, port, channel or cooling. The cause, found
  2026-11-01: heat. The firmware's temperature reading (TEMP_COMP_GET) goes from
  92 to 127, as far as it reads, in about 25 s of sending flat out, and the radio
  then stops both ways for some 40 s; receiving at 119 Mbit/s it settles at 108.
  Its rate control held VHT-MCS 9 at 94-99% success up to the stop, and neither
  its temperature compensation nor 6 dBm instead of 14-18 slowed the rise. The
  driver now reads the temperature every second and caps sending from 116 on
  (aic_data.c): a cool card sends 144 Mbit/s (18.0 MB/s) for the first 20 s or
  so, then what the card can hold -- ten minutes of four streams out without a
  disconnect, 25-31 Mbit/s (3.1-3.8 MB/s) after the first two minutes;
  five 256 MB FTP downloads in a row 54, 38, 29, 28, 24 Mbit/s (6.8 down to 3.1
  MB/s), the rate falling as the whole dongle warms. The
  RTL8822BU on the same router ran the same load for 20 minutes (133-139 Mbit/s
  (16.6-17.4 MB/s), 41 rounds) without one disconnect, so the router and the air are not the cause.
  Nor is this driver: both AIC8800D80 dongles on Linux PCs, with aicsemi's own
  driver, against the same router, sent 60-115 Mbit/s for the first 30 s of a 20
  minute upload and 0-1 Mbit/s for the rest, transmitting at VHT-MCS 0, and stayed
  that slow after a pause. The card itself does not hold a sustained upload.
- **RTL8822BU, rows 6-8** (2026-10-30): eight streams in 227-229 Mbit/s (28.4-28.6
  MB/s), near the USB host controller's ~240; a 20 minute soak each way, 32 of 32
  rounds on the card: 160 Mbit/s (20.0-20.1 MB/s) out, 208-210 Mbit/s (26.0-26.2
  MB/s) in. What took them there besides the xnet defaults below: the reorder
  window no longer drops frames it gave up on (1b3e3f9), receive transfers taken
  back when the host controller stops asking for them under heavy sending
  (910f141, 708337c), SM power save disabled in the HT capability so the AP
  sends on two streams, not one (07acc02), 16 frames to a transfer aggregated from
  the first one out (c32d023, one stream out 157 -> 169 Mbit/s), and channel 116:
  channel 100 had turned 28% busy with the console idle and cost 20-30%. Two
  rows of the 2026-10-29 table for comparison, stock xnet: one stream in 51-54
  Mbit/s (6.4-6.8 MB/s), FTP in 97-99 Mbit/s (12.1-12.4 MB/s).
- **The xnet defaults** (system sockets; 2026-10-29): 128 KB receive and send
  buffers (`xnet_rcvbuf_kb`, `xnet_sndbuf_kb`; 0 keeps xnet's 16 / 63 KB), the
  congestion window clamp at 256 KB (`xnet_cwnd_kb`, 0 keeps 64 KB) and 768 KB
  more in the pool system sockets' data comes from (`xnet_pool_kb`, 0 keeps
  xnet's 280 KB). System callers have 16 TCP sockets in all (about nine
  connections beside the listeners); `xnet_tcp_sockets = 64` gives them up to 255
  (57 connections at once measured), but only with `xnet_rcvbuf_kb` sized to the
  pool, about 1 MB over the connections receiving at once: at 128 KB, 32 streams
  into the console fell to 11 Mbit/s and left connections that never closed; at
  32 KB, 32-48 streams carried 286-289 Mbit/s (35.7-36.1 MB/s). Titles (Aurora,
  games) have a pool of their own, 280 KB too; `xnet_title_pool_kb` (off by
  default) enlarges it for every title started after the plugin (Aurora: 1020 KB,
  ran as before); what a title gains from it is not measured yet. Over the RTL8822BU against the stock stack: one stream in 170
  Mbit/s (21.2 MB/s) against 68 (8.5), out 148 Mbit/s (18.5 MB/s) against 117
  (14.6), eight in 202-206 Mbit/s (25.3-25.8 MB/s) against 170 (21.3), four in
  beside four out 91-93 Mbit/s in against 34-37; ftpdll 118-122 Mbit/s (14.8-15.3
  MB/s) in, 113-114 Mbit/s (14.1-14.3 MB/s) out. 30 of 30 cold boots joined; a 20
  minute soak each way, 32 of 32 rounds on the card: 146-151 Mbit/s (18.3-18.9
  MB/s) out, 188-193 Mbit/s (23.5-24.1 MB/s) in. Over the RTL8156B nothing moved
  but two-way traffic (41 / 148 -> 106 / 106 Mbit/s): the wire was at the USB
  ceiling already. What sets the limits: xnet keeps the data of all system
  sockets in one pool, 280 KB as xam makes it (taken before plugins load), and
  without SACK it stops every connection for good once that fills with data
  waiting for a lost segment or an ACK -- over Wi-Fi first through frames the
  reorder window dropped (1b3e3f9), then with large windows both ways; the
  plugin adds to that pool (37b555c). And the console's USB host controller,
  under heavy sending, stops asking for the card's receive transfers: they are
  taken back and queued again after ~130 ms (910f141), not the card reset.
- **A program that runs few connections** can ask for more on its listening
  socket (SO_RCVBUF/SO_SNDBUF before listen()): over Wi-Fi one stream in 171
  Mbit/s (21.4 MB/s) at 255 KB, one out 151 Mbit/s (18.9 MB/s) at 255 KB, eight in
  at 128 KB 194-198 Mbit/s (24.2-24.7 MB/s) -- as long as what it has open, both
  ways, stays within the 270 KB.
- **One stream out of the console** is bound by xnet's congestion window, which it
  clamps to 64 KB (`xnet_cwnd_kb` lifts the clamp, 256 KB by default): it goes from 101 Mbit/s (12.6 MB/s) to 124 Mbit/s (15.5 MB/s) at 128 KB and 134 Mbit/s
  (16.8 MB/s) at 256 KB, with a send buffer as large (`xnet_sndbuf_kb`, or the
  program's own SO_SNDBUF). Those buffers come out of the same 270 KB (above):
  over Wi-Fi four sockets of 128 KB out stopped every connection on the console.
- **Wi-Fi into the console with one stream** is bound by the console's TCP window
  (17.5 KB) over the radio's round trip, not by the driver. With
  `xectl.py set tcp_rcvbuf 0xffff` the RTL8822BU carries 113-115 Mbit/s
  (14.1-14.4 MB/s) with one stream and 157-160 Mbit/s (19.6-20.0 MB/s) with four;
  `xnet_rcvbuf_kb = 63` in the ini does the same for every system socket (64 is
  skipped: xnet takes it as a zero window). Inbound
  figures of 120-128 Mbit/s (15-16 MB/s) for the AIC8800 came from
  that setting, not from the default. Why the RTL8822BU's single stream sits
  below the AIC8800's at the same window (51-54 Mbit/s (6.4-6.8 MB/s) against
62-66 Mbit/s (7.7-8.2 MB/s)) is not measured yet.

## What it does

The card is switched into its CDC-ECM configuration, where its own firmware runs the
MAC, and its PHY is then told over Realtek vendor requests to advertise up to
2.5GBASE-T -- in ECM the firmware alone only offers 10/100. Frames move one per bulk
transfer, 32 receive transfers always queued, 64 transmit slots. In `xnet` mode (the
default) the console's own network stack uses the card instead of its onboard port:
DHCP, XBDM, FTP, titles.

## Measured

Console card on a 1 Gbit/s home router, the PC on the same router at 1 Gbit/s; the
card is on the console's USB 2.0 port (the UDP generator, XBDM getmemex, curl; speedtest.py and ramxfer.py now in xerunner/handoff).

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
`tools/xectl.py prof 5` reads it. During the TCP runs below, single stream:

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
only with `make bench=tcp` and started with `tools/xectl.py cmd 14`; a plain build has
no xam imports at all, so the driver never depends on xam to load. Measured with
`tools/xectl.py bench tcp`, router at 1 Gbit/s:

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
(`tools/xectl.py bench ftp`), and the same path with no disk (`tools/xectl.py bench tcp`):

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
| `aic8800` | `a69c:8d80`, then `a69c:8d81` | AIC8800D80 USB Wi-Fi 6: firmware load (`aic_fw.c`), the Wi-Fi firmware's bring-up, scan and join (`aic_wifi.c`), frames (`aic_data.c`); offered to `src/wireless/` as a `WL_CARD` |

What every Wi-Fi card shares is in `src/wireless/`, and a driver keeps only what is
its chip's: `wireless.c` the networks a scan found and which to pick, and the
`WL_CARD` a driver fills in (scan, join, leave, state); `wpa.c` the WPA2
handshake; `reorder.c` putting a block ack's frames back in order (mac80211's
agg-rx, with the driver saying which frames need it and running the timeout in
its own receive context); `bridge.c` the policy, the only one so far (a `wifi_mode = dashboard` beside it is planned):
the card as a cable that joins the network `xedrivers.ini` names by itself.

`rndis` was first run on a Xiaomi phone tethering over USB (2717:ff80): INITIALIZE,
MAC, filter and link state answered at the first try, the phone packs several
received frames into one transfer (70 in 39), and in xnet mode the console took
an address from the phone's DHCP and served FTP to a client on the phone. The
console sends to a phone at 24.5 MB/s with several frames packed into one
transfer (21.2 one a transfer; tools/xectl.py blast, UDP to the phone's broadcast
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

## Wi-Fi: the AIC8800D80

The chip arrives as `a69c:8d80`, its boot ROM; the driver writes the firmware into
it from the `aic8800D80` directory beside the plugin (`make push-fw` puts Linux's
files there) and starts it, and the chip comes back as `a69c:8d81` with the Wi-Fi
firmware running. Both phases send exactly the messages aicsemi's Linux driver
sends, checked against its own usbmon recording (`tests/drivers/aic8800/`). Scans
fill a table of networks (`tools/xectl.py status`); `xectl.py cmd 31 <index>` joins
one, `cmd 32` leaves. Open networks only so far: a secured one needs the WPA2 key
exchange, which is not written yet, and is refused.

WPA2-Personal works: `wifi_pass = <passphrase>` beside the name in
`xedrivers.ini`. The console does no ciphering itself -- the card's firmware does,
given the keys -- so all the plugin runs is the four-way handshake
(`src/wireless/wpa.c`), and every hash and block operation in it is the console's
own kernel (`XeCryptHmacSha*`, `XeCryptSha*`, `XeCryptAes*`), never written here.
The association asks the firmware to leave the controlled port shut and pass
EAPOL through; the handshake answers the access point's first and third messages,
and the keys it settles are installed from the worker, because a key is a command
the firmware confirms and that cannot be waited for where frames arrive. Only then
is the link reported up, so xnet runs DHCP after the port opens and not before.
The two awkward parts are checked on the console against published examples
(`xectl.py cmd 20`): the key from a passphrase against IEEE 802.11i's worked
example, and the group key's unwrapping against RFC 3394's.

A network can also be joined by itself: `wifi_ssid = <name>[, <name>...]` in
`xedrivers.ini` beside the plugin (one `key = value` a line, `#` for comments).
While the card is up and not joined it scans every five seconds and joins the
strongest access point of any name listed -- which covers two routers under one
name, and the two bands of one router, whose names differ; the same loop brings it back after the access
point goes away (tried: hostapd stopped, the console told of the loss within five
seconds, joined again ten seconds after it returned). `xectl.py cmd 32` leaves and
keeps it off until `cmd 31` joins again.

The station associates on the console's own address, read from its configuration
(category 2, setting 1) before any device is brought up, because that is what xnet
sends from and all it takes in. A Wi-Fi link is an uplink like any other, below a
USB Ethernet card and above the onboard port.

Measured with an open access point on the PC's twin card (Linux's aic8800 driver in
AP mode), 2.4 GHz channel 6 and 5 GHz channel 36 alike, 20 MHz from NetworkManager
or 40-80 MHz from hostapd: one TCP stream 0.8 MB/s each way, four streams 2.0 MB/s
out, eight 2.7 MB/s out (22 Mbit/s, the radio at VHT-MCS 2-4 and -73 dBm) and
1.3 MB/s in. The console is not what limits it. Its TCP
keeps about four segments in flight, which on the wire (0.3 ms round trip) is
21.9 MB/s; over this link a round takes about 7 ms, so 4 x 1460 bytes / 7 ms. Of
those 7 ms, decomposed with usbmon on the access point's own USB: about 4.7 ms the
access point's driver holds four received frames before its IP stack sees them,
about 2.5 ms is the air both ways and the console. Inside the console a received
frame is answered in 22-69 us and a transmit transfer accepted by the chip in
0.25-0.3 ms (`make bench=prof`, stages `turnaround` and `tx_wait`). Turning power
saving off, 24 receive requests instead of 8, larger socket buffers (the console
then offers a 64 KB window) and the firmware's other receive-aggregation setting
each changed the figure by under a tenth.

What the same console and card carry, by access point. Eight TCP streams, the
plugin's own server, the card three metres from the console and a floor below the
house's routers:

| access point | signal | out of the console | into it |
|---|---|---|---|
| hostapd, 5 GHz ch 149, nothing else on it | -62 dBm | 6.0 MB/s | 11.0 MB/s |
| the house's router, 2.4 GHz ch 3 | -64 dBm | 2.9 MB/s | 1.4 MB/s |
| hostapd, 5 GHz ch 36, beside a neighbour | -71 dBm | 0.8 MB/s | 1.3 MB/s |
| the house's router, 5 GHz ch 100, a floor up | -73 dBm | 0.5 MB/s | 0.3 MB/s |

Eight decibels and a free channel are worth twenty times the throughput; nothing
in the driver came close to mattering that much. Which band wins is not obvious
either -- here 2.4 GHz beats the same router's 5 GHz six times over, because it
gets through the floor -- so `wifi_ssid` takes a list and the strongest access
point of any name in it is the one joined.

**A clear channel is worth thirteen times the driver's tuning.** With hostapd on
5 GHz channel 149, which nothing else here uses, WPA2 and CCMP, the same console
and the same card reach 11.0 MB/s into the console and 6.0 MB/s out of it (88 and
48 Mbit/s, eight streams), against 0.8 MB/s on channel 36 next to a neighbour's
network. The signal is -62 dBm rather than -71, and under load the rate holds at
MCS 5-6 instead of collapsing to MCS 0. One stream out of the console is still
1.0 MB/s, because its own stack keeps about four segments in flight and the air
makes a round trip long; into the console one stream already carries 5.9 MB/s.

**Three things were wrong, and two of them were ours.** Measured again with the
card a metre from the access point (hostapd on the PC's twin card, 5 GHz ch 149,
-44 dBm), the 1.0 MB/s out of the console and the losses into it came apart:

- *Receive buffers too small.* The D80's firmware aggregates what it receives into
  one bulk transfer of up to 20480 bytes (`aicwf_usb_rx_aggr` is on for this chip,
  `AICWF_USB_AGGR_MAX_PKT_SIZE`). A 4096-byte buffer ended such a transfer early
  and the rest of it began the next buffer in the middle of a packet: 5381 packets
  cut off in one run. The ring is now eight buffers of 20 KB.
- *No reordering.* The firmware hands an A-MPDU's frames up as they decode, so a
  frame the access point had to retry arrives after the ones behind it -- 3.7% of
  frames into the console under load. xnet answers each with a duplicate ACK, the
  sender fast-retransmits and halves its window: 786 retransmissions in five
  seconds of one stream. `AICWF_RX_REORDER` puts them back in order on Linux, and
  aic_data.c now does the same (a window of 64 per TID, a hole given up on after
  two worker ticks). The same stream then has none, and runs into the console's
  64 KB window instead: 13.0 MB/s, from 7-8.
- *The access point's ACK filter.* aicsemi's Linux driver is built with
  `CONFIG_FILTER_TCP_ACK=y` (aicwf_tcp_ack.c): it holds TCP ACKs up to 5 ms and
  drops the ones a later ACK supersedes -- 331 of 1310 reached the air. A Linux
  sender with a large window does not notice; the console's stack, with a few
  segments in flight, waits 5 ms for every round. That was the 1.0 MB/s out of
  the console, and XBDM's own TCP showed the same 0.8 MB/s. With the module built
  without it (`tests/pc-run.sh aic-swap`), one stream out of the console carries
  12.6 MB/s. This is the PC's access point, not the console: a real router has no
  such filter, but anyone measuring with this card as the access point will hit it.

| plugin's TCP server, MB/s | 1 stream | 4 | 8 | FTP, 64 MB |
|---|---|---|---|---|
| into the console, before | 7.3 | 12.3 | 13.8 | 5.9-9.6 |
| into the console, after | 13.0 (64 KB window) | 13.4-17.5 | 17.4 | 11.1 |
| out of the console, before | 1.0 | 3.5 | 5.9 | 0.97 |
| out of the console, after (no ACK filter at the AP) | 12.6 | 13.3 | 14.0 | 11.2 |

What is left is the chip taking one transmit transfer at a time, 0.16-0.3 ms each
(`tx_wait` busy ~100% with eight streams in): every ACK the console sends is a
transfer of its own. Two ways round it were tried and dropped: several frames
in one transfer stops the firmware transmitting at all (it has a TX_AGGR mode,
but its patch settings exist only for the 8800DC), and merging queued pure ACKs
behind a cap on transfers in flight gained 13% with eight streams and lost the
link with the cap at two.

**Against a real access point it holds.** The PC's card as an access point
degrades under a long load towards the station -- its firmware hung outright
three times with Linux on both ends -- so the last measurements are against an
OpenWrt router (ZBT-WE1326, MT7612 2x2, 5 GHz ch 120 VHT80, wired at 1 Gbit/s)
a metre from the console, signal -25 dBm, VHT-MCS 8-9 at 40 MHz (the station
side settles on 40 MHz under Linux too):

| out of the console | into it | FTP 64 MB, out / in |
|---|---|---|
| 12.9 MB/s one stream, 17.6 four | 8.1 one stream, 13.2 four | 10.8 / 11.2 MB/s |

Twelve back-to-back runs of four streams each way did not move by more than
0.5 MB/s, and three FTP rounds each way came back identical.

**The radio is the limit, not this driver.** The same dongle on the PC, driven by
aicsemi's own Linux driver and associated to the house's router, carries 1.4 MB/s
up and 0.5 MB/s down -- the console's figures. Both dongles see that router within
one dB of each other (-65 and -66 dBm), so neither is the weaker part. What the
rates say is that the link is poor in one direction: the dongle transmits at
MCS 4 (39 Mbit/s) and the router answers it at 6 Mbit/s, the slowest rate 802.11g
has, which is 4 Mbit/s of TCP -- exactly what was measured downwards. The dongle
hears the router at -65 dBm through a floor; the router evidently hears the dongle
much worse, and with 2.4 GHz channels 1, 3, 9 and 11 all occupied here there is
little air left either. Between the two dongles three metres apart the signal is
-67 to -73 dBm, no better than the router a floor away, which is what a chip
antenna in a USB stick gives. Higher figures need the card within a few metres of
the access point, or a clear 5 GHz channel -- not another knob in the driver.

## Bluetooth on the same card

The dongle is one composite device: its first interface is standard HCI over USB
and needs no vendor step once the firmware is in, so the Wi-Fi driver drives it
too, through the same node the kernel handed it (`src/drivers/aic8800/aic_bt.c`).
Commands are class control requests, the controller's answers come back on
interrupt endpoint 0x83. That took one more thing from the core: `USB_DEV` keeps
eight endpoint slots rather than four, which Wi-Fi and Bluetooth together need.

So far it is brought up and asked who it is -- a reset, its version, its address
-- which is what settles whether the console can reach this half at all. On the
bench it answers: address 68:8f:c9:33:9c:7e, one past the Wi-Fi side's, HCI
version 13 revision 11, three commands and three events with no error or timeout
(`xectl.py status`, the `bt` line). Nothing is connected or paired yet; headset
audio would need L2CAP and a codec above this.

## When the card goes

Unplug the card -- or its cable, or end a phone's tethering -- and the console goes
on with its onboard port: without the card's link the link-state hook answers
with the EMAC's, transmit takes the kernel's EMAC path, and the EMAC delivers to
the title itself. Plug it back and it is offered to the plugin again, brought up,
and xnet moves back once its link is up. Measured: the onboard port answered at
once each time; back on the card 4 s after its PHY came up, and at once after a
physical replug of the card.

## Several devices, one uplink

Up to four USB devices are owned at once, each with its driver. Of those with a
link up, one is the uplink xnet is given: a phone that tethers before a USB card,
a faster link before a slower one, the uplink in force before an equal one, two
phones by who tethered first, then the back sockets before the front and a lower
port first; with none, the onboard port. Frames from the others are dropped.
Switching from one device to another gives xnet no link at all for ~1.5 s
first: both read the same to it, and without the gap it kept the old address.

Tried on the console with the RTL8156 cabled and a phone: tethering on -> the
phone's address (10.63.x); off or unplugged -> 192.168.1.210 from the card; card's
cable out -> the onboard port (10.36.x); tethering on again -> the phone; off ->
the onboard port until the card's cable was back. No network test needed at any
step.

Two phones, one in a back socket and one in the front, the card cabled: the one
that tethered first stays the uplink whatever the other does; when it stops, the
other's network (10.18.x) takes over; with both off, the card again. Both phones
had their own slot and RNDIS instance the whole time.

Two RTL8156B (back and front) and the onboard port, all on one 100M switch: every
one of them gets 192.168.1.210, since xnet sends the console's own MAC through
whichever is the uplink and the router answers that MAC. Equal kind and speed, so
the uplink in force keeps it: pulling the uplink's cable moves xnet to the other
card (the gap shows 0.0.0.0 for a moment, then DHCP); pulling the other card's
cable, or the same card's again once it is no longer the uplink, changes nothing.

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

    tools/xectl.py status
    tools/xectl.py mode xnet
    tools/xectl.py advertise 1000
    tools/xectl.py phy 0xa400            read a PHY register (OCP address)
    tools/xectl.py blast 100000          UDP generator: 100000 datagrams to the peer

`rtlreg.py` (xerunner/handoff) reads the same registers on the twin card under Linux, for
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
