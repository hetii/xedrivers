# xedrivers: internals

What all drivers share: the layout, the uplink choice, Wi-Fi, and the limits of the
console itself.

## Layout

| Directory | What |
|---|---|
| `src/core/` | the USB driver object, the worker thread, the command channel, `driver.h` |
| `src/net/` | the network device that xnet and the standalone stack see, the xnet hooks, the uplink choice |
| `src/wireless/` | what every Wi-Fi card shares |
| `src/drivers/<d>/` | one directory per driver, one `XE_DRIVER` each |
| `src/bench/<t>/` | measuring and tuning tools, only with `bench=` |
| `tests/` | host unit tests, beside the code: `tests/core/`, `tests/drivers/<d>/` |

## Drivers

The core reaches a driver only through `src/core/driver.h`: one `XE_DRIVER` named
`xe_driver_<directory>`, with these entries:

| Entry | Called when |
|---|---|
| `match` | the kernel offers a device |
| `start` | the driver took it |
| `detached` | it went away |
| `xmit`, `tx_space` | the network layer sends |
| `command` | optional, a debug command for this driver |

- The Makefile builds the table of drivers from the directories. Adding a card is
  adding a directory; removing one is deleting it.
- Every driver's `match` is asked. One that knows the product (VID:PID) wins over
  one that only knows the class.
- A driver that builds on another includes its public header, and that is the whole
  dependency. `make DRIVERS=rtl8156` without `cdc` stops at that include.
- Every transmit completion must call `net_tx_done()`. Without it xnet's queue
  stalls under load and it looks like a console hang.

## Start-up

1. `DllMain` registers a USBD driver object and starts the worker thread. Nothing
   else: allocating pool there hangs the boot.
2. The worker allocates DMA memory, finds devices already enumerated at boot and
   resets them, so the kernel offers them to the plugin.
3. All `UsbdQueueAsyncTransfer` calls run on hardware thread 2 at DISPATCH_LEVEL.
   The USB stack is not locked against other processors.
4. xam's import thunks for NicXmit, NicGetLinkState, NicAttach and NicDetach point
   at the plugin's hooks. The kernel's originals stay intact.

### The worker and the dimmed screen

About ten minutes after the last pad input the dashboard dims the screen. From then
on a thread waiting on hardware thread 2 is not woken until a pad is touched.

- ExCreateThread puts the worker there, beside the USB stack.
- Frames kept flowing (they are completions), but a card plugged in was not brought
  up and the notification endpoint was not re-armed.
- The worker now runs on hardware thread 5 (`WORKER_PROCESSOR`), where it keeps
  running.
- It waits on an event set by a device arriving or leaving and by a finished control
  transfer, instead of sleeping in fixed steps.

Why thread 2 sleeps through the dim is not known.

## Uplinks

Up to four USB devices are owned at once, each by its driver. Of those with a link
up, one is the uplink xnet uses. The order:

1. a phone that tethers, before a USB card;
2. a faster link before a slower one;
3. the current uplink before an equal one;
4. two phones: the one that tethered first;
5. the back sockets before the front, a lower port first.

With none, the onboard port. Frames from the other devices are dropped.

Switching from one device to another gives xnet no link at all for about 1.5 s
first. Both look the same to xnet, and without the gap it kept the old address.

When the uplink goes (card or cable pulled, tethering ended):

- the link-state hook answers with the onboard port's link;
- transmit takes the kernel's EMAC path;
- the onboard port answered at once each time.

Back on the card 4 s after its PHY came up, and at once after a replug.

Two RTL8156B and the onboard port on one switch all get 192.168.1.210: xnet sends
the console's own MAC through whichever is the uplink. Pulling the uplink's cable
moves xnet to the other card. Pulling the other card's cable changes nothing.

## Wi-Fi

`src/wireless/` holds what every Wi-Fi card shares. A driver keeps only what is its
chip's.

| File | What |
|---|---|
| `wireless.c` | the networks a scan found, which to pick, the `WL_CARD` a driver fills in (scan, join, leave, state), `xedrivers.ini` |
| `wpa.c` | the WPA2 four-way handshake, with the kernel's own crypto |
| `reorder.c` | block-ack reordering (as mac80211's agg-rx); the driver says which frames need it |
| `bridge.c` | `wifi_mode` default: the card as a cable to the network `wifi_ssid` names |
| `omni.c` | `wifi_mode = dashboard`: the card as the console's own wireless adapter |
| `softmac/` | a SoftMAC station for cards without a full-MAC firmware (the RTL8822BU) |

A Wi-Fi link is an uplink like any other: below a USB Ethernet card, above the
onboard port.

## Console limits

### USB

| Direction | Ceiling | Measured with |
|---|---|---|
| out of the console | 23.3 MB/s at the Ethernet MTU | both RTL8156B and RTL8822BU stop at 21 to 24 MB/s |
| into the console | at least 36 MB/s | RTL8822BU, 32 to 48 streams, about 18 KB transfers |

Outbound, sweeping the frame size fits 10.9 us a transfer plus 36.9 ns a byte (78
to 1486 byte frames). The per-byte term dominates, so packing more frames into a
transfer moves nothing out.

The EHCI controllers (0x7FEA3000 and 0x7FEA5000, read with `xectl.py cmd 16`):

| Register | EHCI 0 | EHCI 1 | Means |
|---|---|---|---|
| HCSPARAMS | 0x00101404 | 0x00101505 | 4 and 5 ports, one USB 1.1 companion each |
| HCCPARAMS | 0x7070 | 0x7070 | no asynchronous park mode, 32-bit only |
| USBCMD | 0x00010031 | 0x00010021 | running, async schedule on, interrupt threshold 1 microframe |

The two knobs an EHCI driver has for bulk throughput are where they can go. With
the transmit queue full and the processor 90% idle, what is left is the controller
fetching 512-byte packets: about 6.6 a microframe out, of the 13 USB 2.0 allows.

### Processor

Measured with `make bench=prof` on the RTL8156B, one stream:

- Out, a frame leaves every 66.5 us and costs the processor about 7 us. The copy
  into the transfer buffer is 8.6% of a thread. Copying less would move nothing.
- In, the USB thread is 21% busy, mostly in xnet's own TCP receive.

### Socket servers

A thread that serves sockets needs priority 19 or more (20 is used). At the default
priority, and at 18 and 24, TCP falls to 36 Mbit/s (4.5 MB/s) out and 20 Mbit/s
(2.5 MB/s) in. 19, 20, 22, 28 and 31 all give the full rate.

xnet sockets also need SOL_SOCKET option 0x5802, then 0x5801 = 1, or xnet drops
SYNs.

### xnet

The xnet settings and why their defaults are what they are: README,
Configuration. The measurements are in the comments of `src/net/xnet.c`.
