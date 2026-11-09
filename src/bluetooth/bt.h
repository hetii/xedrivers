/* The console's Bluetooth host: whatever controller a driver brings up (an AIC8800's Bluetooth
 * half, later a plain USB dongle) is handed over as a transport, and from there on this side
 * speaks HCI with it -- and, profile by profile, to the devices it finds (audio/, later hid/).
 *
 * The transport runs at any IRQL; the host itself only on the worker thread (bt_tick): what the
 * controller sends is queued from the USB completion and taken in order there.
 */

#ifndef XE_BT_H
#define XE_BT_H

#include "kernel.h"

/* A controller, as a driver hands it over. Both calls are thread-only and copy what they are
 * given; 0 if sent. */
typedef struct {
    void *ctx;
    u32 (*command)(void *ctx, const u8 *packet, u32 length);   /* an HCI command packet */
    u32 (*acl)(void *ctx, const u8 *packet, u32 length);       /* an ACL data packet */
} BT_TRANSPORT;

/* The driver's side. attach and tick from the worker; detach, event and acl at any IRQL up
 * to DISPATCH. One controller at a time. */
void bt_attach(const BT_TRANSPORT *t);
void bt_detach(void);
void bt_event(const u8 *event, u32 length);         /* a whole HCI event */
void bt_acl_in(const u8 *packet, u32 length);       /* a whole ACL packet */
void bt_tick(void);

/* A host command (xectl.py bt): BT_CMD_SCAN starts a scan of ~10 s. 1 if handled. */
#define BT_CMD_SCAN 40u
u32 bt_command(u32 what, volatile u32 *args);

#define BT_FOUND_MAX 16u

/* What a scan found, read by tools/xectl.py. */
typedef struct {
    u8  address[6];             /* as written, most significant first */
    u8  pad[2];
    u32 device_class;
    s32 rssi;                   /* dBm, 0 when the controller gave none */
    u32 seen;                   /* answers counted */
    char name[32];
} BT_FOUND;

typedef struct {
    u32 attached;
    u32 state;                  /* host.h, BT_STATE_* */
    u32 commands;
    u32 events;
    u32 events_dropped;         /* the queue was full */
    u32 last_opcode;
    u32 last_status;            /* of the last command that reported one */
    u32 failed_opcode;          /* the last one that did not succeed */
    u32 address[2];             /* the controller's, bytes in order */
    u32 acl_mtu;
    u32 acl_slots;
    u32 inquiries;
    u32 found;
} BT_STATS;

extern volatile BT_STATS bt_stats;
extern volatile BT_FOUND bt_found[BT_FOUND_MAX];

#endif /* XE_BT_H */
