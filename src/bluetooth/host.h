/* The host's own state, shared by its files (hci.c now; l2cap.c, pair.c and the profiles
 * later). Worker thread only, but for the event queue's producer side. */

#ifndef XE_BT_HOST_H
#define XE_BT_HOST_H

#include "bt.h"

#define BT_STATE_DETACHED   0u
#define BT_STATE_INIT       1u      /* the bring-up commands, one after another */
#define BT_STATE_READY      2u
#define BT_STATE_INQUIRY    3u

/* opcodes: the group (OGF) above 10 bits of the command (OCF) */
#define HCI_OP(ogf, ocf)    ((ogf) << 10 | (ocf))
#define HCI_INQUIRY             HCI_OP(0x01, 0x0001)
#define HCI_RESET               HCI_OP(0x03, 0x0003)
#define HCI_SET_EVENT_MASK      HCI_OP(0x03, 0x0001)
#define HCI_WRITE_LOCAL_NAME    HCI_OP(0x03, 0x0013)
#define HCI_WRITE_SCAN_ENABLE   HCI_OP(0x03, 0x001A)
#define HCI_WRITE_CLASS         HCI_OP(0x03, 0x0024)
#define HCI_WRITE_INQUIRY_MODE  HCI_OP(0x03, 0x0045)
#define HCI_WRITE_SSP_MODE      HCI_OP(0x03, 0x0056)
#define HCI_READ_BUFFER_SIZE    HCI_OP(0x04, 0x0005)
#define HCI_READ_BD_ADDR        HCI_OP(0x04, 0x0009)

#define HCI_EV_INQUIRY_COMPLETE     0x01u
#define HCI_EV_INQUIRY_RESULT       0x02u
#define HCI_EV_COMMAND_COMPLETE     0x0Eu
#define HCI_EV_COMMAND_STATUS       0x0Fu
#define HCI_EV_INQUIRY_RESULT_RSSI  0x22u
#define HCI_EV_EXTENDED_INQUIRY     0x2Fu

/* Send one command now (the controller has a slot): `params` of `length`. 0 if sent. */
u32  hci_send(u32 opcode, const u8 *params, u32 length);

/* Parsing, pure (tests/bluetooth): what an inquiry result event says about each device in it,
 * into bt_found; and the name in an extended inquiry response. */
void hci_inquiry_result(const u8 *event, u32 length);
u32  hci_eir_name(const u8 *eir, u32 length, char *name, u32 size);

#endif /* XE_BT_HOST_H */
