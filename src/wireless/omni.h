/* The card as the console's own wireless adapter: `wifi_mode = dashboard`. It is
 * registered with the kernel as a wireless network device (NicRegisterDevice), the
 * way the console's own adapters are, and the dashboard drives it: it scans, lists
 * the networks, takes the passphrase and joins, and the console's network stack
 * sends and receives through it. Nothing about the network comes from
 * xedrivers.ini; the network layer is told the card is not a cable (net_claim), so
 * xnet never takes it for its uplink.
 *
 * What the dashboard asks, and the form of every answer, was recorded with the
 * probe in src/bench/debug/omni_probe.c (xerunner: dashboard-wireless-join@1) and
 * read off the kernel's own driver for the console's adapters (omni,
 * xerunner: omni-options@1, omni-services-17559@1).
 *
 * There is one such device in the kernel at a time, so this is one too: a second
 * card in this mode is not registered while the first is.
 */

#ifndef XE_WL_OMNI_H
#define XE_WL_OMNI_H

#include "wireless.h"
#include "net.h"

/* Read by tests/xectl.py. */
typedef struct {
    u32 registered;         /* 1, or what NicRegisterDevice refused with */
    u32 attached;           /* the kernel made it its wireless device */
    u32 present;            /* the card is up; 0 tells the dashboard there is none */
    u32 link;
    u32 gets;
    u32 sets;
    u32 unknown_gets;
    u32 unknown_sets;
    u32 last_unknown;       /* the last option not answered */
    u32 scans;
    u32 joins;
    u32 missed;             /* asked to join a network the last scan did not see */
    u32 leaves;
    u32 rejoins;            /* joined again after losing the network */
    u32 rx;
    u32 tx;
    u32 tx_dropped;
    u32 tx_high;
    u32 security;           /* as the dashboard set it (11002) */
} WL_OMNI_STATS;

extern volatile WL_OMNI_STATS wl_omni_stats;

/* The card is up: register (once) and take `nd` from the network layer. Thread. */
void wl_omni_start(const WL_CARD *card, NET_DEV *nd);

/* Every worker tick while the card is up: what the dashboard asked for is done
 * here, where the card may be waited for. */
void wl_omni_tick(void);

/* The card is gone. Any IRQL up to DISPATCH. The device stays registered and says
 * there is no adapter until a card is started again. */
void wl_omni_gone(void);

#endif /* XE_WL_OMNI_H */
