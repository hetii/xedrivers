/* Giving the card to the console's own network, in place of the onboard EMAC.
 *
 * xnet lives in xam and reaches the wired NIC through xam's import thunks for the
 * kernel's Nic* exports. This module re-points those thunks at hooks in the plugin:
 * a frame the title sends becomes a USB transfer, a frame off the card becomes a
 * call to the title's receive callback, and the link the title polls is the card's.
 * The kernel's own Nic* functions are never touched, so every other caller and the
 * onboard EMAC keep working; disabling restores the thunks word for word.
 *
 * Addresses are specific to this build (retail 2.0.17559, xam from the 21256
 * recovery, loaded at 0x815F0000). xnet_enable verifies every thunk still decodes
 * to the kernel Nic function it expects before it patches anything, and refuses on
 * any mismatch -- so a different build is declined, not corrupted.
 *
 * Modelled on proferabg/CUsbEnet (GPL-2.0), the published USB-Ethernet driver for
 * this console, reduced to the retail title path.
 */

#ifndef XE_XNET_H
#define XE_XNET_H

#include "kernel.h"

/* Re-point xam's Nic* thunks at the hooks and attach to the title's NIC user.
 * Returns 0, or a non-zero step code if a thunk did not match (XNET_ERR_*). */
u32  xnet_enable(void);

/* Restore every thunk this patched. Safe to call when not enabled. */
void xnet_disable(void);

/* Have the title's xnet ask for the link again, as on a change of it. */
void xnet_link_changed(void);

enum {
    XNET_OK = 0,
    XNET_ERR_XMIT_THUNK = 1,
    XNET_ERR_LINK_THUNK = 2,
    XNET_ERR_NO_USER = 3,
};

typedef struct {
    u32 enabled;
    u32 patched;            /* thunks currently redirected */
    u32 user;               /* the title CNicUser we deliver to */
    u32 xmit_usb;           /* frames sent over USB via the hook */
    u32 xmit_fallback;      /* frames handed to the kernel EMAC path */
    u32 rx_delivered;       /* frames handed up to the title */
    u32 rx_filtered;        /* frames the title's filter rejected */
    u32 rx_no_user;
    u32 link_polls;
    u32 flags_before;
    u32 flags_after;
    u32 link_notifies;
    u32 xmit_dropped;       /* completed to xnet unsent: queue full or link gone */
    u32 xq_high;            /* the deepest the transmit queue has been */
    u32 link_fallback;      /* link polls answered with the EMAC's: card down */
} XNET_STATS;

extern volatile u32 xnet_rx_iface;

extern volatile XNET_STATS xnet_stats;

#endif /* XE_XNET_H */
