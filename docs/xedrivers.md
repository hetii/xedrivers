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
| `src/drivers/common/<name>/` | code shared by one maker's drivers, not drivers (everything under `common/`); what each driver needs is `NEEDS_<driver>` in the Makefile |
| `src/bench/<t>/` | measuring and tuning tools, only with `bench=` |
| `src/servers/<s>/` | file servers ([xenfs](servers/xenfs.md), [xeftp](servers/xeftp.md)), only with `servers=`; they use nothing but `src/servers/common/srv.h` |
| `src/servers/common/` | the servers' platform: `srv.h`, its console side (`srv_net.c`, `srv_file.c`, `srv_sys.c`), `srv_keep.c`, `str.h`; the host side is in `tests/servers/common/` |
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
| `wpa.c` | the WPA2 four-way and group key handshakes, with the kernel's own crypto |
| `reorder.c` | block-ack reordering (as mac80211's agg-rx); the driver says which frames need it |
| `bridge.c` | `wifi_mode` default: the card as a cable to the network `wifi_ssid` names |
| `omni.c` | `wifi_mode = dashboard`: the card as the console's own wireless adapter |
| `softmac/` | a SoftMAC station for cards without a full-MAC firmware (the RTL8822BU) |

A Wi-Fi link is an uplink like any other: below a USB Ethernet card, above the
onboard port.

## Calling between gcc code and the kernel

The plugin is gcc code in `-m32`, the kernel Microsoft's in 64-bit registers. Two differences
have stopped the console, and neither shows on a normal bench run:

- **The kernel stores arguments in its caller's frame.** Home area SP+0x10 to +0x4F (`kit/kcall.S`).
  gcc keeps saved registers there. The OHCI side of `UsbdQueueAsyncTransfer` does it, so that call
  goes through `usb_framed_call`; nothing else in 0x800d8000-0x800de000 does. A new kernel call
  into code not read yet can do the same.
- **gcc saves r14-r31 with `stw` and `lwz`.** A DPC runs on the thread it interrupted, whose r14-r31
  are live and 64 bits wide, and the upper half comes back zero: a game then crashes anywhere. So
  everything the kernel calls at DISPATCH_LEVEL enters through a stub that saves them with `std`:
  `usb_prepare` and `usb_isoch` do it for completions, `usb_dpc_init` for DPCs, `USB_STUB` for the
  rest. Never give the kernel a plugin function directly for a DPC or a completion.

To see a hang: `bench_trace` marks and `xerunner flash read-uart`, which also shows the system's
crash dumps. `flash power --off` switches off a frozen console. Boot several times: one boot proves
little.

## Console limits

### USB

| Direction | Ceiling | Measured with |
|---|---|---|
| out of the console | about 24 MB/s | RTL8156B NCM 190 Mbit/s (23.8 MB/s), RTL8822BU 179 Mbit/s (22.4 MB/s), with the NAK reload |
| into the console | not known; at least 28 MB/s | RTL8156B 27 to 28 MB/s (NCM 27.9), RTL8822BU 25 to 26 MB/s at 8 to 32 streams |

Outbound, sweeping the frame size fits 10.9 us a transfer plus 36.9 ns a byte (78
to 1486 byte frames). The per-byte term dominates, so packing more frames into a
transfer moves nothing out.

The RTL8822BU's 286 to 289 Mbit/s (36 MB/s) in, at 32 to 48 streams, came from a
bench tool that counted every stream over its own window (fixed 2026-11-02, `xectl
bench tcp` now counts one window, up at the console). The RTL8156B, counted the same
wrong way, read 344 Mbit/s where the console received 220. Counted right, the
RTL8822BU carries 205 Mbit/s (25.6 MB/s) in at 32 streams.


### Transfer descriptors

The kernel's EHCI driver takes its transfer descriptors (qTDs) from one free list for
every controller and device, two or more a request, and never looks whether the list has
run out: with none left it writes through a null pointer at DISPATCH_LEVEL and the console
stops (`PANIC_STACK_SWITCH`, then `ATTEMPTED_SWITCH_FROM_DPC` on the UART). The pool is
about 250. One card's rings fit in it; two RTL8156B, an AIC8800 and an RTL8822BU together
left 124 free when idle, and the first bulk send out of the console used them up
(2026-11-11: every `bench tcp down` and an ftpdll download ended that way). It is also
what the old rule "64 receive and 128 send requests outstanding hang the console" was.

The core now counts the free list before it queues a request (src/core/usb.c, qtd_room)
and keeps what does not fit waiting, in order, until completions give descriptors back;
`xectl.py status` says how many requests waited (`usb qtd`). The list's head is at
0x801A7F80 in kernel 17559; the address is believed only when the code that loads it
reads as expected, so on another kernel there is no guard. With the four cards: 170
Mbit/s (21.3 MB/s) out and 177 (22.1) in on the uplink, where it stopped before.

### Two cards at once

The console's USB ports are on two host controllers, and each has its own ceiling
(2026-11-11, bench=stack: xnet's TCP into one card, 4 streams, while the bench's UDP
stack on a second card, `xectl.py stack SLOT IP`, counts what a paced sender gives it;
the PC's Ethernet flow control off, or the pause frames one card causes hold the PC's
whole port and the second card starves):

| TCP into (xnet) | UDP into (stack) | Controllers | TCP | UDP | Sum |
|---|---|---|---|---|---|
| RTL8156B | none | | 186 Mbit/s (23.3 MB/s) | | 186 Mbit/s (23.3 MB/s) |
| RTL8156B | RTL8156B | two | 187 Mbit/s (23.4 MB/s) | 188 Mbit/s (23.5 MB/s) | 375 Mbit/s (46.9 MB/s) |
| RTL8156B | AIC8800 | two | 186 Mbit/s (23.3 MB/s) | 173 Mbit/s (21.6 MB/s) | 359 Mbit/s (44.9 MB/s) |
| RTL8156B | RTL8822BU | one | 157 Mbit/s (19.6 MB/s) | 116 Mbit/s (14.5 MB/s) | 273 Mbit/s (34.1 MB/s) |

Alone, the stack counted 188 Mbit/s (23.5 MB/s) on the RTL8156B, 173 (21.7) on the AIC8800
and 235 (29.4) on the RTL8822BU. So a second card on the other controller nearly doubles
what comes in, and on the same controller the two share about 34 MB/s. The UDP side has
no xnet, TCP or disk behind it: what two TCP paths through xnet cost is not in these figures.

### The kernel's USB stack

Read off xboxkrnl 21256 (retail, with its PDB) and found the same in the
console's 17559:

- `UsbdEhciQueueAsyncTransfer` chains a queued transfer's qTDs behind the ones
  already out on the endpoint, so the hardware runs a queue with no gap; a
  transfer over 20 KB becomes several qTDs, each with interrupt-on-complete.
- When a bulk or control endpoint's queue drains, the kernel unlinks its queue
  head (`UsbdEhciRemoveAndFlushEndpoint`, an async-advance doorbell). Keeping it
  linked instead (a one-word patch, tried live) made receiving 9% slower: without
  park mode every linked queue head, idle too, costs each pass of the schedule.
- It leaves the NAK count reload (RL) of every queue head at 0, so a receive
  endpoint that answers NAK is retried on every pass. `usb_open` sets RL 1 on
  high-speed bulk endpoints (`src/core/usb.c`): RTL8822BU out 168 -> 179 Mbit/s
  (21.0 -> 22.4 MB/s), ftpdll into the console 118 -> 130 Mbit/s; RTL8156B out 175 ->
  181-186 (ECM) and 179 -> 190 (NCM). Receiving is unchanged on both.
- Which hardware thread a server's threads run on (the default, the USB stack's
  2, or 3, 4, 5) changes nothing; pinning one to 1, the title's, hung the console.
- How deep the receive queue is does not matter: 8, 16 or 32 transfers of 20 KB
  on the RTL8822BU gave the same.
- The EHCI controllers are PCI devices 4 and 5 on bus 1, function 1 (OHCI function
  0), vendor 0x1414. HAL (`HalpWriteEhciOperationalMode`) sets vendor registers
  0x40 (20 40 00 0c), 0x44 (3c on this southbridge revision, 04 on older ones) and
  0x48 (ad 03 00 40; 6e 00 00 00 older), and on revision 0x80 and up PHY registers
  through 0x7C. Read and set live with `make bench=debug` (command 24,
  `HalReadWritePCISpace`): 0x44 = 04 changed nothing, 7f gave in +2% and out -4%;
  0x48's older value and ff in its first byte changed nothing; ff in its last byte
  hung the console until a power cycle. Nothing there to gain.

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
