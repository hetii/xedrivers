/* Bluetooth HCI over USB, the standard every dongle and the AIC8800's Bluetooth half keep to
 * (USB class e0/01/01): commands as class control requests, events on an interrupt IN
 * endpoint, ACL data on a bulk pair, SCO on the next interface's isochronous pair (later).
 * A driver that owns such a device calls btusb_start; from there the host (src/bluetooth)
 * has it as a transport.
 */

#ifndef XE_BTUSB_H
#define XE_BTUSB_H

#include "usb.h"
#include "bt.h"

#define BTUSB_EVENT_MAX 260u        /* an event: 2 header bytes and up to 255 */
#define BTUSB_ACL_MAX   1028u       /* an ACL packet: 4 header bytes and up to 1024 */
#define BTUSB_OUT       4u          /* ACL packets on their way out at once */

/* Where the HCI interface's endpoints are, from the configuration descriptor. */
typedef struct {
    u32 interface;
    u32 event, event_max, event_interval;
    u32 acl_in, acl_out, acl_max;
} BTUSB_EPS;

typedef struct {
    u32 started;
    u32 events;
    u32 event_errors;
    u32 acl_in_packets;
    u32 acl_out_packets;
    u32 acl_out_full;
    u32 commands;
    u32 command_errors;
    u32 last_error;
    u32 start_status;           /* of the last btusb_start */
    u32 interface;              /* what btusb_find found there */
    u32 ep_event, ep_acl_in, ep_acl_out, acl_max;
} BTUSB_STATS;

typedef struct BTUSB {
    USB_DEV *dev;
    BTUSB_EPS eps;
    volatile BTUSB_STATS *st;
    void *ep_event, *ep_acl_in, *ep_acl_out;
    volatile u32 running;
    u32 errors;
    URB event_urb;
    u8 *event_buf;                  /* DMA: one interrupt transfer */
    u8 event[BTUSB_EVENT_MAX];      /* an event put together from transfers */
    u32 event_have;
    URB acl_urb;
    u8 *acl_buf;                    /* DMA: one bulk transfer */
    u8 acl[BTUSB_ACL_MAX];
    u32 acl_have;
    volatile u32 acl_receiving;     /* the ACL IN request is out */
    URB out_urb[BTUSB_OUT];
    u8 *out_buf[BTUSB_OUT];
    volatile u32 out_busy[BTUSB_OUT];
    u8 *cmd_buf;
    BT_TRANSPORT transport;
} BTUSB;

extern volatile BTUSB_STATS btusb_stats;

/* Pure (tests/bluetooth): the HCI interface in a configuration descriptor. 1 if found. */
u32  btusb_find(const u8 *config, u32 length, BTUSB_EPS *eps);

/* Worker thread: read the device's configuration, open the HCI endpoints, start receiving,
 * hand the controller to the host. `taken`, if not 0, lists endpoint addresses (0-ended) the
 * device's other functions use already: the HCI interface must not share one. 0, or what
 * failed (BTUSB_START_*). */
#define BTUSB_START_CONFIG    1u
#define BTUSB_START_NOT_HCI   2u
#define BTUSB_START_MEMORY    3u
#define BTUSB_START_ENDPOINTS 4u
#define BTUSB_START_SHARED    5u
u32  btusb_start(BTUSB *b, USB_DEV *dev, const u8 *taken);

/* The device is going (DISPATCH_LEVEL is fine): stop asking, tell the host. */
void btusb_stop(BTUSB *b);

#endif /* XE_BTUSB_H */
