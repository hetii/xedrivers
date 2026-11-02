# cdc: CDC ECM and NCM

Directory: `src/drivers/cdc/`

| File | What |
|---|---|
| `cdc.c` | parses the CDC descriptors |
| `cdc_net.c` | runs the device: transfers, the interrupt endpoint, ECM and NCM framing |

## Matching

`cdc` matches by class: a CDC ECM or NCM interface in the device's current
configuration. A driver that knows the product (VID:PID) wins over it.

`cdc` alone does not take an RTL8156. The kernel offers a device in its first
configuration, which on that card is Realtek's own protocol, so `match` never sees
a CDC interface. Measured: `make DRIVERS=cdc` leaves the card unclaimed. The
`rtl8156` driver builds on `cdc` for it ([rtl8156.md](rtl8156.md)).

## Transports

| | ECM (default) | NCM |
|---|---|---|
| Frames per transfer | 1 | several in one NTB (`NTB_SIZE`, 16 KB) |
| Receive transfers queued | 32 (`RX_COUNT`) | 16 (`NCM_RX_COUNT`) |
| Transmit slots | 64 (`TX_COUNT`) | 16 (`NCM_TX_COUNT`) |

The transport is chosen at build time (`TRANSPORT=ecm|ncm`). On a running console
`tools/xectl.py transport ecm|ncm` switches it and resets the card.

## The interrupt endpoint

The RTL8156 in ECM answers every poll of its notification endpoint with the same
two notifications, CONNECTION_SPEED_CHANGE and NETWORK_CONNECTION, in turn. Linux
`cdc_ether` sees the same at the 128 ms its bInterval asks for.

What was wrong here:

- `UsbdOpenEndpoint` takes the interval in milliseconds and polls at least every
  32 ms.
- The high-speed bInterval (an exponent, 11) was passed raw: 62.5 transfers a
  second.

What the driver does now:

- converts bInterval (32 ms);
- parks the endpoint for about a second after a notification identical to the last
  of its kind;
- acts on a change and re-arms at once.

Result: 1.0 transfer a second, a pulled cable seen within 0.9 s. TCP out stayed at
175 Mbit/s (21.9 MB/s) at any notification rate; this spares the bus and the USB
stack, not the throughput.

## Issues

- **NCM is not the default.** On the RTL8156B it gains 3 to 6% in and about 3% out
  at eight streams or more, but is slower with one stream at 128 KB buffers. Older
  measurements are in the comments of `cdc_net.c`.
- **`cdc_rx_cpu` stays 0.** Moving receive to another processor (`cdc_rx_cpu` 1)
  once stopped the console's network until a power cycle, under the plugin's TCP
  server at full rate. With FTP every value worked but gained nothing.
- **Only the RTL8156B has been tested** as a CDC device so far.
- **RX 64 with TX 128 outstanding requests hangs the console.** Keep the counts as
  they are.
