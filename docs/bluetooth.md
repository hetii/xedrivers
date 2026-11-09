# Bluetooth

The console's own Bluetooth host, for a controller on USB (today the AIC8800D80's
Bluetooth half). Goal: headphones with microphone (A2DP, HFP), later pads (HID).

## Layout

    src/drivers/common/btusb/   HCI over USB (class e0/01/01): commands as class control
                                requests, events on the interrupt IN endpoint, ACL on bulk
    src/bluetooth/bt.h          the transport a driver hands over; the host's stats
    src/bluetooth/host.h        shared by the host's files
    src/bluetooth/hci.c         bring-up, command queue (credits), events, inquiry, ACL credits
    src/bluetooth/pair.c        links both ways, Secure Simple Pairing (Just Works), link keys
    src/bluetooth/keys.c        bt_pair lines of xedrivers.ini
    src/bluetooth/l2cap.c       basic-mode channels both ways, configuration, echo, info
    src/bluetooth/sdp.c         SDP server: the A2DP source record
    src/bluetooth/profiles.c    what starts on a secure link
    src/bluetooth/audio/sbc.c   SBC encoder, 8 subbands, every mode (SBC, SBC-XQ)
    src/bluetooth/audio/a2dp.c  AVDTP both roles, the SBC choice, RTP stream paced by the time base
    later: audio/{rfcomm,hfp}.c, hid/, le/

Events are queued from the USB completion and taken on the worker (`bt_tick`). The ACL bulk
IN is asked for only while a link is up: queued from the start (before the HCI reset), it
halved an RTL8822BU's throughput on the same EHCI (180/194 -> 67/94 Mbit/s, TCP x4 out/in),
link or not; asked for at the first link it costs nothing measurable, A2DP streaming
included (179.9/198.7). `bluetooth = off` in the ini leaves the controller alone.

## Bring-up

Reset, Read_BD_ADDR, Read_Buffer_Size, Set_Event_Mask, class of device 0x000804,
name "Xbox 360", inquiry mode 2 (RSSI / extended results), Simple Pairing on, page scan.

## Keys

A pairing's link key is kept in RAM and, from xedrivers.ini, read back at start:

    bt_pair  = C6:52:BA:8A:3F:4F 0123456789ABCDEF0123456789ABCDEF audio
    bt_pair1 = ...                                   (up to bt_pair7)

The key as BlueZ's /var/lib/bluetooth/<adapter>/<device>/info writes it. Writing the ini
from the plugin: later.

## Audio

A2DP source: after a link we made turns secure, DISCOVER, GET_ALL_CAPABILITIES, then
SET_CONFIGURATION with the most bits a second the sink's SBC capabilities allow (48 kHz if
taken, dual channel over joint at the same bitpool, 16 blocks, loudness), OPEN, the media
channel, START. A headset that connects by itself finds the source in SDP and leads; it is
answered as SEID 1 taking any SBC configuration (bitpool up to 76).

## Control

    xectl.py bt                  state, counters, scan results, links, keys (never their value)
    xectl.py bt scan             ~10 s of inquiry (cmd 40), then the list
    xectl.py bt connect N|ADDR   link, pair or prove the key, then A2DP (cmd 41)
    xectl.py bt disconnect N     (cmd 42)

## Status

Stage 3 (2026-11-09): A2DP to the PC's AX200 as the sink: SDP answered, AVDTP both roles,
48 kHz joint stereo bitpool 58 (387 kbit/s), RTP of 5 frames a packet, no missed credits;
the test tone in both channels, no gaps with a USB card playing at the same time. After a
reboot the PC reconnected by itself with the key from the ini. Open: HFP (RFCOMM, SCO over
the isochronous interface, mSBC/CVSD), a real headset, BlueZ refuses Just Works
re-pairing of a device it has a key for (JustWorksRepairing = never) -- hence keys kept.

Stage 2 (2026-11-09): paired with the PC's AX200 (BlueZ: Paired, Bonded), connected again
with the kept key (encrypted, no pairing), L2CAP echo both ways. SBC decoded by gst
sbcdec at 65 dB SNR in every mode.

Stage 1 (2026-11-09): bring-up and inquiry on the AIC8800D80: the PC's AX200 found with
class, RSSI and its name from the extended inquiry response. Next: connection, pairing
(link key kept as `bt_pair` in xedrivers.ini), L2CAP, SDP, A2DP SBC, HFP.
