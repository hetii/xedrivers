# xedrivers

## Overview

xedrivers is a DashLaunch plugin for the Xbox 360. It drives USB devices that the
console has no driver for.

Today these are network devices:

- USB Ethernet adapters,
- Android phones that tether over USB,
- USB Wi-Fi dongles.

The plugin hands each device to the console's own network stack (xnet, in xam) in
place of the onboard port. Everything that uses the network then runs over the USB
device unchanged: DHCP, XBDM, FTP servers, the dashboard and games.

One plugin holds every driver. When the kernel offers a USB device, the driver that
knows it takes it. Up to four devices can be owned at once. The best one by kind
and speed becomes the uplink. Without any, the console falls back to its onboard
port.

Requirements on the console: retail kernel 2.0.17559 and DashLaunch.

How the plugin is built inside (layout, drivers, uplinks, Wi-Fi, the console's
limits): [docs/xedrivers.md](docs/xedrivers.md).

## Supported cards

| # | VID:PID | Card name | Card type | Issues |
|---|---|---|---|---|
| 1 | by class | any CDC ECM or NCM device | USB Ethernet | [cdc.md](docs/drivers/cdc.md#issues) |
| 2 | `2717:ff80` | Xiaomi phone, Android USB tethering | RNDIS | [rndis.md](docs/drivers/rndis.md#issues) |
| 3 | `0bda:8156` | Realtek RTL8156 / RTL8156B | USB 2.5G Ethernet | [rtl8156.md](docs/drivers/rtl8156.md#issues) |
| 4 | `0bda:8153` and OEM ids | Realtek RTL8153 / RTL8153B / RTL8153C (not yet run on hardware) | USB gigabit Ethernet | [rtl8153.md](docs/drivers/rtl8153.md#issues) |
| 5 | `0b95:1790`, `0b95:178a` and OEM ids | ASIX AX88179 / AX88178A / AX88179A/B (not yet run on hardware) | USB gigabit Ethernet | [ax88179.md](docs/drivers/ax88179.md#issues) |
| 6 | `a69c:8d80`, `a69c:8d81` | AICSemi AIC8800D80 | USB Wi-Fi 6 and Bluetooth | [aic8800.md](docs/drivers/aic8800.md#issues) |
| 7 | `0bda:b812` | Realtek RTL8822BU | USB Wi-Fi 5 | [rtw8822b.md](docs/drivers/rtw8822b.md#issues) |

The RNDIS driver matches by class. Row 2 is the phone that was tested; other
Android phones that tether over RNDIS should work too.

## Building

### What you need

- `gcc-powerpc-linux-gnu`, `python3`, `curl` and `sha256sum`. A build and the unit
  tests need nothing else: the packer and the ordinal tables are in `kit/`, and the
  firmware is fetched (see below).
- [xerunner](../xerunner), only to work with a running console (`tools/xectl.py`,
  `make deploy`, `make kit`).

Environment for working with a console:

| Variable | Meaning |
|---|---|
| `XE_HOST` | the console's address (required by `tools/xectl.py`) |
| `XERUNNER` | the xerunner CLI; default `xerunner` on PATH, then `../xerunner` |
| `XERUNNER_HOME` | the xerunner tree, for `make kit` |

### Build

On Debian or Ubuntu:

```sh
sudo apt install gcc-powerpc-linux-gnu python3 make curl
make            # the package: build/xedrivers/
make test       # host unit tests
make lint       # cppcheck and clang-tidy
```

`make` builds the whole package, ready to copy to the console:

```text
build/xedrivers/
    xedrivers.xex
    firmware/
        rtw88/rtw8822b_fw.bin
        aic8800D80/...
```

Before building, it checks every firmware file against the SHA-256 pinned in the
Makefile (`FIRMWARE`). A missing or different file is fetched from its source at a
fixed commit:

| Firmware | Source |
|---|---|
| RTL8822BU | [linux-firmware](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git) |
| AIC8800D80 | AICSemi's USB driver package, as [radxa-pkg/aic8800](https://github.com/radxa-pkg/aic8800) keeps it |

Nothing is taken from the machine's own `/lib/firmware`, so every build packs the
same files. The pins are the versions the drivers were tested with.

Options:

```sh
make DRIVERS=cdc,rtl8156     # only the drivers named (default: all)
make TRANSPORT=ncm           # CDC transport: ecm (default) or ncm
make ADVERTISE=1000          # fastest Ethernet link offered: 2500 (default), 1000, 100, default
make bench=tcp               # with the bench tools named, see below
make bench=all               # with every bench tool
make MODE=standalone IP=10.36.0.2 bench=stack
```

`MODE` says where received frames go:

| `MODE` | Frames go to |
|---|---|
| `xnet` (default) | the console's network stack, in place of the onboard port |
| `standalone` | a built-in responder on `IP`: ARP, ping, UDP echo (7), discard (9) and a UDP generator. The console's own network is untouched. Needs `bench=stack`. |
| `drop` | nowhere; the device is only brought up |

Lab targets (they need xerunner and the bench described in `CLAUDE.md`):

```sh
make kit                     # regenerate kit/ from xerunner, for a new kernel build
make push PI=root@host       # copy the package's plugin to a USB-gadget medium
make push-fw PI=root@host    # copy the package's firmware there
make deploy HOST=...         # push, then boot it with xerunner console test-plugin
```

A release build is made by GitHub Actions (`.github/workflows/build.yml`): every
run keeps the package as an artifact, and a tag `v*` publishes it as a release.

Every build records the commit it was made from. `tools/xectl.py status` shows it
as "built from", with a mark when the tree had uncommitted changes.

### The bench

The bench is the set of measuring and tuning tools in `src/bench/<tool>/`.

- A plain `make` builds the plugin for people, with none of them.
- A tool is built in only when named with `bench=`.
- Use them while developing or measuring a driver. Never ship a build with them.

| `bench=` | What it adds | Used by |
|---|---|---|
| `history` | flight recorder: frames, bytes, in flight and drops every 200 ms, about 100 s | `xectl.py history` |
| `stack` | the standalone responder and the UDP generator | `xectl.py blast` |
| `debug` | commands 15 to 17: thread priority by id, read and write any word (MMIO too) | `xectl.py cmd 15/16/17` |
| `prof` | timers for each stage of a frame (hooks in `src/core/prof.h`) | `xectl.py prof` |
| `tcp` | a RAM-to-RAM TCP server on the console; the plugin's only imports from xam | `xectl.py bench tcp\|sockets` |

The core reaches the tools only through `src/core/bench.h`. Without any tool it is
empty inline functions.

## Installing

Without xerunner, by hand:

1. Get the package: a release's zip, or `build/xedrivers/` from `make`.
2. Copy the whole `xedrivers` directory into DashLaunch's `plugin` directory:
   - on a USB stick: `Usb:\plugin\xedrivers\`, or
   - on the hard disk: `Hdd:\plugin\xedrivers\`.

   Any FTP server on the console works (ftpdll, Aurora's), or write the stick on a
   PC.
3. The firmware is already in `xedrivers\firmware`. That directory is laid out as
   linux-firmware, so newer files from Linux can replace it, or any of its
   subdirectories, as they are. They must be uncompressed: some distributions keep
   `/lib/firmware` as `.zst` (unpack with `zstd -d`).
4. Put `xedrivers.ini` beside the plugin if needed (see Configuration). Wi-Fi needs
   it to know which network to join.
5. Add the plugin to DashLaunch's `launch.ini`, on a free plugin slot:

   ```ini
   [Plugins]
   plugin3 = Usb:\plugin\xedrivers\xedrivers.xex
   ```

6. Restart the console.

The result on a USB stick:

```text
Usb:\plugin\xedrivers\
    xedrivers.xex
    xedrivers.ini
    firmware\
        rtw88\rtw8822b_fw.bin
        aic8800D80\fmacfw_8800d80_u02.bin, ...
```

Files are looked for first in the plugin's own directory, then in
`\plugin\xedrivers` on the USB devices and on the hard disk.

> Load a new or untested build from `Usb:` only. If it hangs the boot, pull the
> stick and the console starts without it. A plugin that hangs the boot from `Hdd:`
> cannot be removed remotely.

## Configuration

`xedrivers.ini` is read from the plugin's directory at start.

- One `key = value` per line.
- Lines starting with `#` or `;` are comments.
- Every key is optional; a missing key takes its default.

Example:

```ini
wifi_ssid = home_5g, home_2.4g
wifi_pass = passphrase
```

### Wi-Fi

| Key | Default | What it does |
|---|---|---|
| `wifi_ssid` | none | Networks to join, comma-separated. |
| `wifi_pass` | none | WPA2-Personal passphrase. Without it only open networks are joined. |
| `wifi_mode` | bridge | `dashboard` offers the card to the dashboard as the console's own wireless adapter. Anything else makes the card work like a cable to a network from `wifi_ssid`. |

How `wifi_ssid` is used:

- While the card is up and not joined, it scans every 5 s.
- It joins the strongest access point of any name listed.
- The same loop joins again after the access point goes away.
- A list covers two routers with one name and the two bands of one router.

### xnet

These keys change the console's network stack (xnet in xam 17559) for system
programs: the dashboard, XBDM, FTP servers and plugins. Each change first checks
the code or data it patches and is skipped if that is not what it expects. `0`
keeps xnet's own value.

| Key | Default | xnet's own | Limit |
|---|---|---|---|
| `xnet_rcvbuf_kb` | 128 | 16 | 255; 64 is refused |
| `xnet_sndbuf_kb` | 128 | 63 | 255 |
| `xnet_cwnd_kb` | 256 | 64 | 4096, a multiple of 64 |
| `xnet_pool_kb` | 768 (added) | pool of 280 | 2048 |
| `xnet_tcp_sockets` | 0 (keep 16) | 16 | 255 |
| `xnet_title_pool_kb` | 0 (keep 280) | 280 | 1020 |

Why these defaults:

- **Receive buffer, 128 KB.** One TCP stream into the console over Wi-Fi went from
  68 Mbit/s (8.5 MB/s) to 170 Mbit/s (21.2 MB/s). 64 is refused because xnet then
  advertises a zero window.
- **Send buffer, 128 KB.** One stream out went from 117 Mbit/s (14.6 MB/s) to
  148 Mbit/s (18.5 MB/s).
- **Congestion window cap, 256 KB.** xnet never has more than 64 KB in flight on one
  connection. Over Wi-Fi's round trip that is about 110 Mbit/s whatever the
  buffers. The cap only matters when the send buffer is that large too.
- **Pool, +768 KB.** All system sockets keep their data in one pool. With the stock
  280 KB, 128 KB buffers stopped every connection once four went each way.
- **Sockets, stock 16.** 16 sockets are about nine connections beside the
  listeners. More are possible, but the stock limit is also what keeps the receive
  windows inside the pool. At 128 KB, 32 streams in fell to 11 Mbit/s and left
  connections that never closed. Raise it only with `xnet_rcvbuf_kb` sized so all
  connections receiving at once fit in about 1 MB (32 KB for 32 streams).
- **Titles' pool, off.** Every title started after the plugin (Aurora, games) gets
  its own pool of 280 KB. This key enlarges it, at the cost of the title's memory.
  The gain is not measured yet.

xnet has no SACK. When its pool fills with data waiting for a lost segment, the
segment that would free it is dropped too, and every connection stops. That is why
the buffers and the pool are raised together. The measurements are in the comments
of `src/net/xnet.c`.

A program with few connections can ask for more on its listening socket
(`SO_RCVBUF` and `SO_SNDBUF` before `listen()`), up to 255 KB. All its open
buffers, both ways, must still fit in the pool.

### At run time

`tools/xectl.py` changes a running plugin over XBDM, with no rebuild or reboot:

```sh
export XE_HOST=192.168.1.210
tools/xectl.py status                  # state, link, counters, "built from"
tools/xectl.py mode xnet|standalone|drop
tools/xectl.py transport ecm|ncm       # resets the card to apply
tools/xectl.py advertise 2500|1000|100|default
tools/xectl.py set|get SYMBOL [VALUE]  # any 32-bit global of the plugin
```

## Speedtests

`tools/xectl.py bench` measures throughput between the PC and the console:

```sh
export XE_HOST=192.168.1.210
tools/xectl.py cmd 14                       # start the TCP server (bench=tcp)
tools/xectl.py bench tcp --streams 1,4,8 --repeat 3
tools/xectl.py bench ftp both --mb 256 --login xboxftp:xboxftp
tools/xectl.py bench sockets --count 80     # how many connections the console holds
```

### TCP and FTP

| | TCP (`bench tcp`) | FTP (`bench ftp`) |
|---|---|---|
| Server | the plugin's own (`make bench=tcp`), port 5201 sends, 5202 receives | the console's FTP server (ftpdll) |
| Data | RAM to RAM | a file to and from `Hdd1` |
| Shows | what the card, the driver and xnet carry | what a user gets copying games |
| Run | 10 s, 1 or more parallel streams | one file each way, upload first |

FTP is lower than TCP: ftpdll receives and writes the disk in turn on one thread.

How it counts: every stream runs in one common window. Console -> PC is what the
PC received in it; PC -> console is what the console's server received, read at
both ends of the window. Checked on 2026-11-02 against the PC kernel's own TCP
counters and the console's: they agree within 0.1%. Results from before that date
with 8 or more streams were counted per stream and read high.

### Keeping results

Every result is set against the one on file under the same conditions: card,
transport or Wi-Fi channel, xnet settings, test, streams and direction. With
`--record` it is kept in `docs/speedtest.jsonl`:

| New result against the one on file | What happens |
|---|---|
| better by more than `--tolerance` (3%) | it replaces the old one |
| within the tolerance | nothing changes |
| worse by more | refused as a possible regression, unless `--accept-lower REASON` |

- A record names the commit the plugin was built from.
- A build with uncommitted changes cannot be recorded.
- `--repeat N` takes the median of N runs.
- `git log -p docs/speedtest.jsonl` keeps the older figures.
- `tools/results.py` prints the tables; `tools/results.py --readme` writes them below.

<!-- results:wired -->
| Card | Transport, link | xnet | Test | Streams | Console -> PC | PC -> console | Date, commit | Note |
|---|---|---|---|---|---|---|---|---|
| rtl8156 | ECM, 1000 Mbit/s | rcv 128 / snd 128 / cwnd 256 KB, pool +767 KB, 16 TCP sockets | ftp 256 MB | 1 | 166 Mbit/s (20.7 MB/s) | 164 Mbit/s (20.5 MB/s) | 2026-11-03, e500b723 / 2026-11-02, 58b851a |  |
| rtl8156 | ECM, 1000 Mbit/s | rcv 128 / snd 128 / cwnd 256 KB, pool +767 KB, 16 TCP sockets | tcp ram | 1 | 186 Mbit/s (23.3 MB/s) | 212 Mbit/s (26.6 MB/s) | 2026-11-03, e500b723 / 2026-11-02, cc7b35d3 |  |
| rtl8156 | ECM, 1000 Mbit/s | rcv 128 / snd 128 / cwnd 256 KB, pool +767 KB, 16 TCP sockets | tcp ram | 4 | 182 Mbit/s (22.7 MB/s) | 214 Mbit/s (26.8 MB/s) | 2026-11-03, e500b723 / 2026-11-02, cc7b35d3 |  |
| rtl8156 | ECM, 1000 Mbit/s | rcv 128 / snd 128 / cwnd 256 KB, pool +767 KB, 16 TCP sockets | tcp ram | 8 | 181 Mbit/s (22.7 MB/s) | 214 Mbit/s (26.8 MB/s) | 2026-11-03, e500b723 / 2026-11-02, cc7b35d3 |  |
| rtl8156 | ECM, 1000 Mbit/s | rcv 32 / snd 128 / cwnd 256 KB, pool +767 KB, 64 TCP sockets | tcp ram | 32 | 175 Mbit/s (21.8 MB/s) | 212 Mbit/s (26.5 MB/s) | 2026-11-02, cc7b35d3 / 2026-11-02, cc7b35d3 |  |
| rtl8156 | NCM, 1000 Mbit/s | rcv 128 / snd 128 / cwnd 256 KB, pool +767 KB, 16 TCP sockets | ftp 256 MB | 1 | 168 Mbit/s (21.0 MB/s) | 145 Mbit/s (18.1 MB/s) | 2026-11-03, e500b723 / 2026-11-03, 61f08246 |  |
| rtl8156 | NCM, 1000 Mbit/s | rcv 128 / snd 128 / cwnd 256 KB, pool +767 KB, 16 TCP sockets | tcp ram | 1 | 193 Mbit/s (24.2 MB/s) | 217 Mbit/s (27.1 MB/s) | 2026-11-03, e500b723 / 2026-11-02, cc7b35d3 |  |
| rtl8156 | NCM, 1000 Mbit/s | rcv 128 / snd 128 / cwnd 256 KB, pool +767 KB, 16 TCP sockets | tcp ram | 4 | 190 Mbit/s (23.8 MB/s) | 222 Mbit/s (27.8 MB/s) | 2026-11-03, e500b723 / 2026-11-02, cc7b35d3 |  |
| rtl8156 | NCM, 1000 Mbit/s | rcv 128 / snd 128 / cwnd 256 KB, pool +767 KB, 16 TCP sockets | tcp ram | 8 | 190 Mbit/s (23.8 MB/s) | 223 Mbit/s (27.9 MB/s) | 2026-11-03, e500b723 / 2026-11-02, cc7b35d3 |  |
| rtl8156 | NCM, 1000 Mbit/s | rcv 32 / snd 128 / cwnd 256 KB, pool +767 KB, 64 TCP sockets | tcp ram | 32 | 181 Mbit/s (22.6 MB/s) | 220 Mbit/s (27.5 MB/s) | 2026-11-02, cc7b35d3 / 2026-11-02, cc7b35d3 |  |
<!-- /results:wired -->

<!-- results:wifi -->
| Card | Band, channel, width | xnet | Test | Streams | Console -> PC | PC -> console | Date, commit | Note |
|---|---|---|---|---|---|---|---|---|
| aic8800 | 5 GHz, ch 116, 40 MHz | rcv 128 / snd 128 / cwnd 256 KB, pool +767 KB, 16 TCP sockets | ftp 256 MB | 1 | 11 Mbit/s (1.4 MB/s) | 84 Mbit/s (10.4 MB/s) | 2026-11-03, 0f32c18e / 2026-11-03, 0f32c18e |  |
| aic8800 | 5 GHz, ch 116, 40 MHz | rcv 128 / snd 128 / cwnd 256 KB, pool +767 KB, 16 TCP sockets | tcp ram | 1 | 121 Mbit/s (15.1 MB/s) | 114 Mbit/s (14.3 MB/s) | 2026-11-03, 0f32c18e / 2026-11-03, 0f32c18e |  |
| aic8800 | 5 GHz, ch 116, 40 MHz | rcv 128 / snd 128 / cwnd 256 KB, pool +767 KB, 16 TCP sockets | tcp ram | 4 | 65 Mbit/s (8.1 MB/s) | 108 Mbit/s (13.5 MB/s) | 2026-11-03, 0f32c18e / 2026-11-03, 0f32c18e |  |
| rtw8822b | 2.4 GHz, ch 10, 40 MHz | rcv 128 / snd 128 / cwnd 256 KB, pool +767 KB, 16 TCP sockets | ftp 256 MB | 1 | 90 Mbit/s (11.2 MB/s) | 90 Mbit/s (11.2 MB/s) | 2026-11-03, 87a6f44a / 2026-11-03, 87a6f44a |  |
| rtw8822b | 2.4 GHz, ch 10, 40 MHz | rcv 128 / snd 128 / cwnd 256 KB, pool +767 KB, 16 TCP sockets | tcp ram | 1 | 123 Mbit/s (15.4 MB/s) | 130 Mbit/s (16.2 MB/s) | 2026-11-03, 87a6f44a / 2026-11-03, 87a6f44a |  |
| rtw8822b | 2.4 GHz, ch 10, 40 MHz | rcv 128 / snd 128 / cwnd 256 KB, pool +767 KB, 16 TCP sockets | tcp ram | 4 | 145 Mbit/s (18.2 MB/s) | 151 Mbit/s (18.9 MB/s) | 2026-11-03, 87a6f44a / 2026-11-03, 87a6f44a |  |
| rtw8822b | 2.4 GHz, ch 10, 40 MHz | rcv 128 / snd 128 / cwnd 256 KB, pool +767 KB, 16 TCP sockets | tcp ram | 8 | 156 Mbit/s (19.5 MB/s) | 132 Mbit/s (16.5 MB/s) | 2026-11-03, 87a6f44a / 2026-11-02, cc7b35d3 |  |
| rtw8822b | 5 GHz, ch 116, 80 MHz | rcv 128 / snd 128 / cwnd 256 KB, pool +767 KB, 16 TCP sockets | ftp 256 MB | 1 | 110 Mbit/s (13.8 MB/s) | 130 Mbit/s (16.2 MB/s) | 2026-11-02, cc7b35d3 / 2026-11-03, 53ca8d10 |  |
| rtw8822b | 5 GHz, ch 116, 80 MHz | rcv 128 / snd 128 / cwnd 256 KB, pool +767 KB, 16 TCP sockets | tcp ram | 1 | 178 Mbit/s (22.2 MB/s) | 165 Mbit/s (20.7 MB/s) | 2026-11-03, 53ca8d10 / 2026-11-02, cc7b35d3 |  |
| rtw8822b | 5 GHz, ch 116, 80 MHz | rcv 128 / snd 128 / cwnd 256 KB, pool +767 KB, 16 TCP sockets | tcp ram | 4 | 179 Mbit/s (22.4 MB/s) | 198 Mbit/s (24.7 MB/s) | 2026-11-03, 53ca8d10 / 2026-11-02, cc7b35d3 |  |
| rtw8822b | 5 GHz, ch 116, 80 MHz | rcv 128 / snd 128 / cwnd 256 KB, pool +767 KB, 16 TCP sockets | tcp ram | 8 | 178 Mbit/s (22.2 MB/s) | 209 Mbit/s (26.2 MB/s) | 2026-11-03, 53ca8d10 / 2026-11-03, 53ca8d10 |  |
| rtw8822b | 5 GHz, ch 116, 80 MHz | rcv 32 / snd 128 / cwnd 256 KB, pool +767 KB, 64 TCP sockets | tcp ram | 8 | 158 Mbit/s (19.8 MB/s) | 189 Mbit/s (23.6 MB/s) | 2026-11-02, cc7b35d3 / 2026-11-02, cc7b35d3 |  |
| rtw8822b | 5 GHz, ch 116, 80 MHz | rcv 32 / snd 128 / cwnd 256 KB, pool +767 KB, 64 TCP sockets | tcp ram | 32 | 152 Mbit/s (19.0 MB/s) | 205 Mbit/s (25.6 MB/s) | 2026-11-02, cc7b35d3 / 2026-11-02, cc7b35d3 |  |
<!-- /results:wifi -->

## Credits

- [proferabg/CUsbEnet](https://github.com/proferabg/CUsbEnet) (GPL-2.0): the model
  for putting a USB NIC under xnet.
- Linux `r8152` (GPL-2.0): the RTL8156 register interface.
- Linux `cdc_ether`, `cdc_ncm` and `rndis_host`: the CDC and RNDIS transports.
- Linux `rtw88` (dual BSD/GPL): the RTL8822BU bring-up and its register tables.
- AICSemi's Linux `aic8800` driver: the AIC8800D80 firmware load and messages.
- Linux `mac80211`: block-ack reordering.
- The xerunner knowledge base: the kernel's USB driver interface, xam's network
  stack, the ordinals and the packer in `kit/`.
