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
#define HCI_CREATE_CONNECTION   HCI_OP(0x01, 0x0005)
#define HCI_DISCONNECT          HCI_OP(0x01, 0x0006)
#define HCI_ACCEPT_CONNECTION   HCI_OP(0x01, 0x0009)
#define HCI_LINK_KEY_REPLY      HCI_OP(0x01, 0x000B)
#define HCI_LINK_KEY_NEGATIVE   HCI_OP(0x01, 0x000C)
#define HCI_PIN_CODE_REPLY      HCI_OP(0x01, 0x000D)
#define HCI_AUTH_REQUESTED      HCI_OP(0x01, 0x0011)
#define HCI_SET_ENCRYPTION      HCI_OP(0x01, 0x0013)
#define HCI_IO_CAPABILITY_REPLY HCI_OP(0x01, 0x002B)
#define HCI_USER_CONFIRMATION_REPLY HCI_OP(0x01, 0x002C)
#define HCI_SETUP_SYNC          HCI_OP(0x01, 0x0028)
#define HCI_ACCEPT_SYNC         HCI_OP(0x01, 0x0029)
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
#define HCI_EV_CONNECTION_COMPLETE  0x03u
#define HCI_EV_CONNECTION_REQUEST   0x04u
#define HCI_EV_DISCONNECTION_COMPLETE 0x05u
#define HCI_EV_AUTH_COMPLETE        0x06u
#define HCI_EV_ENCRYPTION_CHANGE    0x08u
#define HCI_EV_NUMBER_COMPLETED     0x13u
#define HCI_EV_PIN_CODE_REQUEST     0x16u
#define HCI_EV_LINK_KEY_REQUEST     0x17u
#define HCI_EV_LINK_KEY_NOTIFICATION 0x18u
#define HCI_EV_IO_CAPABILITY_REQUEST 0x31u
#define HCI_EV_USER_CONFIRMATION_REQUEST 0x33u
#define HCI_EV_SYNC_COMPLETE        0x2Cu
#define HCI_EV_COMMAND_COMPLETE     0x0Eu
#define HCI_EV_COMMAND_STATUS       0x0Fu
#define HCI_EV_INQUIRY_RESULT_RSSI  0x22u
#define HCI_EV_EXTENDED_INQUIRY     0x2Fu

/* Send one command now (the controller has a slot): `params` of `length`. 0 if sent. */
u32  hci_send(u32 opcode, const u8 *params, u32 length);

/* The transport's voice data on or off (BT_TRANSPORT.sco); 0 if done. */
u32  hci_sco(u32 on);

/* Send it when the controller has a slot, in order. 0 if queued. */
u32  hci_queue(u32 opcode, const u8 *params, u32 length);

/* --- connections (pair.c) ---------------------------------------------------------------- */

#define BT_CONN_MAX 4u
#define BT_KEY_MAX  8u

#define BT_CONN_FREE        0u
#define BT_CONN_PAGING      1u      /* asked for, or a request accepted */
#define BT_CONN_UP          2u      /* the ACL link is there */
#define BT_CONN_AUTH        3u      /* pairing or proving the key */
#define BT_CONN_ENCRYPTING  4u
#define BT_CONN_SECURE      5u      /* encrypted: the profiles may start */

#define BT_KIND_AUDIO 1u
#define BT_KIND_HID   2u

/* Read by tools/xectl.py: keep it in words. */
typedef struct {
    u8  address[6];             /* as written */
    u8  pad[2];
    u32 state;
    u32 handle;
    u32 we_called;
    u32 encrypted;
    u32 connects;
    u32 last_status;            /* of the last event about it */
} BT_CONN;

typedef struct {
    u8  address[6];
    u8  pad[2];
    u8  key[16];                /* as the controller gives and takes it */
    u32 type;
    u32 used;
} BT_KEY;

extern volatile BT_CONN bt_conns[BT_CONN_MAX];
extern volatile BT_KEY bt_keys[BT_KEY_MAX];
extern volatile u32 bt_keys_changed;

void bt_pair_event(const u8 *event, u32 length);
u32  bt_connect(const u8 *address);
u32  bt_disconnect(volatile BT_CONN *c);
volatile BT_CONN *bt_conn_by_address(const u8 *address);
volatile BT_CONN *bt_conn_by_handle(u32 handle);
volatile BT_KEY *bt_key_for(const u8 *address);
void bt_key_store(const u8 *address, const u8 *key, u32 type);
void bt_key_forget(const volatile u8 *address);

/* One bt_pair line of xedrivers.ini, pure: 1 if it read. */
u32  bt_parse_pair(const char *line, u8 *address, u8 *key, u32 *kind);

/* keys.c, thread-only: the bt_pair lines into bt_keys. */
void bt_load_keys(void);
u32  bt_save_keys(void);                /* 0 if written */

/* --- ACL (hci.c) ---------------------------------------------------------------------- */

/* One ACL packet, whole, to link `handle`: `data` is the L2CAP frame. 0 if sent (there was
 * a credit). */
u32  hci_acl_send(u32 handle, const u8 *data, u32 length);

/* --- L2CAP (l2cap.c) -------------------------------------------------------------------- */

#define L2CAP_FREE       0u
#define L2CAP_CONNECTING 1u
#define L2CAP_CONFIG     2u
#define L2CAP_OPEN       3u
#define L2CAP_CLOSING    4u

struct L2CAP_CHANNEL;

/* A profile's side of its channels; any may be 0. Worker thread. */
typedef struct {
    void (*open)(struct L2CAP_CHANNEL *ch);
    void (*data)(struct L2CAP_CHANNEL *ch, const u8 *data, u32 length);
    void (*closed)(struct L2CAP_CHANNEL *ch, u32 was_open);
} L2CAP_USER;

typedef struct L2CAP_CHANNEL {
    u32 state;
    volatile BT_CONN *conn;
    u16 psm;
    u16 local_cid;
    u16 remote_cid;
    u16 remote_mtu;
    u8  ident;
    u8  config;
    u16 last_result;
    const L2CAP_USER *user;
    void *ctx;
} L2CAP_CHANNEL;

typedef struct {
    u32 frames_in;
    u32 frames_out;
    u32 signals_in;
    u32 signals_out;
    u32 send_failures;
    u32 incoming;
    u32 refused;
    u32 opened;
    u32 rejected;
    u32 echo_asked;
    u32 echo_answered;
    u32 no_channel;
    u32 no_channel_in;
    u32 too_long;
    u32 last_refused_psm;
} L2CAP_STATS;

extern volatile L2CAP_STATS l2cap_stats;
extern L2CAP_CHANNEL l2cap_channels[8];          /* l2cap.c CHANNELS */

u32  l2cap_listen(u32 psm, const L2CAP_USER *u, void *ctx);
L2CAP_CHANNEL *l2cap_open(volatile BT_CONN *c, u32 psm, const L2CAP_USER *u, void *ctx);
u32  l2cap_send(L2CAP_CHANNEL *ch, const u8 *data, u32 length);
void l2cap_close(L2CAP_CHANNEL *ch);
u32  l2cap_echo(volatile BT_CONN *c);
void l2cap_acl(const u8 *packet, u32 length);       /* a whole ACL packet in */

/* a link is secure, or gone (pair.c -> l2cap.c -> the profiles) */
void bt_l2cap_ready(volatile BT_CONN *c);
void bt_l2cap_gone(volatile BT_CONN *c);

/* profiles.c: what starts on a secure link, and stops with it. */
void bt_profiles_init(void);           /* a controller attached */
void bt_profiles_ready(volatile BT_CONN *c);
void bt_profiles_gone(volatile BT_CONN *c);
void bt_profiles_tick(void);
void bt_profiles_event(const u8 *event, u32 length);  /* events the profiles want too */
u32  bt_profiles_command(u32 what);                   /* bt.h BT_CMD_SCO */

typedef struct {
    u32 requests;
    u32 errors;
    u32 continued;
} SDP_STATS;

typedef struct {
    u32 commands;
    u32 not_implemented;
    u32 presses;                /* PASS THROUGH pressed */
    u32 last_button;            /* its operation id, bit 7 released */
} AVRCP_STATS;

extern volatile AVRCP_STATS avrcp_stats;
void avrcp_init(void);

extern volatile SDP_STATS sdp_stats;

/* Parsing, pure (tests/bluetooth): what an inquiry result event says about each device in it,
 * into bt_found; and the name in an extended inquiry response. */
void hci_inquiry_result(const u8 *event, u32 length);
u32  hci_eir_name(const u8 *eir, u32 length, char *name, u32 size);

#endif /* XE_BT_HOST_H */
