# Bluetooth

The console's own Bluetooth host, for a controller on USB (today the AIC8800D80's
Bluetooth half). Goal: headphones with microphone (A2DP, HFP), later pads (HID).

## Layout

    src/drivers/common/btusb/   HCI over USB (class e0/01/01): commands as class control
                                requests, events on the interrupt IN endpoint, ACL on bulk
    src/bluetooth/bt.h          the transport a driver hands over; the host's stats
    src/bluetooth/host.h        shared by the host's files
    src/bluetooth/hci.c         bring-up, command flow (credits), events, inquiry
    later: l2cap.c, sdp.c, pair.c, audio/{avdtp,a2dp,sbc,rfcomm,hfp}.c, hid/, le/

Events are queued from the USB completion and taken on the worker (`bt_tick`).

## Bring-up

Reset, Read_BD_ADDR, Read_Buffer_Size, Set_Event_Mask, class of device 0x000804,
name "Xbox 360", inquiry mode 2 (RSSI / extended results), Simple Pairing on, page scan.

## Control

    xectl.py bt          state, counters, what the last scan found
    xectl.py bt scan     ~10 s of inquiry (cmd 40), then the list

## Status

Stage 1 (2026-11-09): bring-up and inquiry on the AIC8800D80: the PC's AX200 found with
class, RSSI and its name from the extended inquiry response. Next: connection, pairing
(link key kept as `bt_pair` in xedrivers.ini), L2CAP, SDP, A2DP SBC, HFP.
