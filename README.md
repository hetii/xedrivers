# xedrivers

A driver for the Realtek RTL8156/RTL8156B USB 2.5G Ethernet adapter (`0bda:8156`) on
the Xbox 360, delivered as a DashLaunch plugin. Built on Linux with nothing but
`gcc-powerpc-linux-gnu` and `python3`: the ordinals and the packer are in `kit/`,
copied from xerunner.

## What it does

The card is switched into its CDC-ECM configuration, where its own firmware runs the
MAC, and its PHY is then told over Realtek vendor requests to advertise up to
2.5GBASE-T -- in ECM the firmware alone only offers 10/100. Frames move one per bulk
transfer, 32 receive transfers always queued, 64 transmit slots.

Measured between two RTL8156B cards cabled directly, both on USB 2.0 ports, the link
negotiated at 2500 Mbit/s (`tests/speedtest.py`):

| direction | rate | notes |
|---|---|---|
| PC -> console | 242 Mbit/s | the USB 2.0 ceiling: more senders only add loss |
| console -> PC | 188 Mbit/s | 15.5k frames/s, one frame per USB transfer |
| ping | 0.27 ms | |

## Building

    make                                   standalone, advertise 2500, 10.36.0.2
    make MODE=xnet                         the console's own network uses the card
    make MODE=standalone ADVERTISE=1000 IP=192.168.50.2
    make test                              host unit tests
    make push                              put it on the Usb: medium (Orange Pi)

## Modes

| `MODE` | frames go to |
|---|---|
| `standalone` | a built-in responder on `IP`: ARP, ping, UDP echo (7), discard (9), and a UDP generator for throughput runs. The console's own network is untouched. |
| `xnet` | the console's network stack, in place of the onboard EMAC |
| `drop` | nowhere (bring the card up only) |

`ADVERTISE` is the fastest link offered: `2500`, `1000`, `100`, or `default` (the
card's firmware choice).

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
