# rndis: Android phones tethering over USB

Directory: `src/drivers/rndis/` · by class · RNDIS

| File | What |
|---|---|
| `rndis.c` | the RNDIS messages |
| `rndis_net.c` | the driver: transfers, packing, link state |

## Matching

By class: an RNDIS control interface, one of E0/01/03, 02/02/FF or EF/04/01.
Tested with a Xiaomi phone (`2717:ff80`).

## How it works

- INITIALIZE, the MAC address, the packet filter and the link state are asked as
  Linux `rndis_host` does. The phone answered all of them at the first try.
- Receive transfers are offered at 2048 bytes (`rndis_rx_max`), as in Linux. The
  phone then sends one frame a transfer.
- Transmit packs several frames into one transfer.
- The phone enables tethering only once the console is up. That is fine: the kernel
  offers the phone to the plugin when it arrives.

As an uplink a tethering phone comes before a USB card. Of two phones, the one that
tethered first keeps the uplink ([xedrivers.md](../xedrivers.md#uplinks)).

## Measured

- xnet took an address from the phone's DHCP, and the console served FTP to a
  client on the phone.
- Console to phone, UDP to the phone's broadcast address (`tools/xectl.py blast`):

  | Transmit | Rate |
  |---|---|
  | several frames a transfer | 196 Mbit/s (24.5 MB/s) |
  | one frame a transfer | 170 Mbit/s (21.2 MB/s) |

- An 80 MB and a 500 MB file went both ways: 714k frames in and 642k out without an
  error.

## Issues

- **Large receive transfers make uploads from the phone crawl.** Offered 16 KB, the
  phone held received frames back to fill the transfer (70 frames in 39), and an FTP
  upload ran at 0.6 to 0.7 Mbit/s (75 to 90 KB/s). Hence 2048 bytes.
- **No TCP or FTP figures from `xectl.py bench` yet.**
