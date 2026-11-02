/* The card as a cable: `wifi_mode = ethernet`, the default. The network and its
 * passphrase come from xedrivers.ini, the card joins it by itself and stays joined,
 * and the network layer sees one more Ethernet uplink; see bridge.c. */

#ifndef XE_WL_BRIDGE_H
#define XE_WL_BRIDGE_H

#include "wireless.h"

/* Read by tools/xectl.py. */
typedef struct {
    u32 scans;              /* scans the automatic join started */
    u32 joins;              /* joins it asked for */
    u32 missed;             /* scans that did not see the configured network */
    u32 secured;            /* ... or saw it secured with nothing this joins */
} WL_BRIDGE_STATS;

extern volatile WL_BRIDGE_STATS wl_bridge_stats;

/* The card is up (the settings read): join at the next tick. */
void wl_bridge_start(WL_BRIDGE *b, const WL_CARD *card);

/* Every worker tick while the card is up. */
void wl_bridge_tick(WL_BRIDGE *b, const WL_CARD *card);

/* By command (tools/xectl.py): join the scan table's entry `index` with the
 * configured passphrase, or leave and stay off until joined again. */
u32 wl_bridge_join(WL_BRIDGE *b, const WL_CARD *card, u32 index);
u32 wl_bridge_leave(WL_BRIDGE *b, const WL_CARD *card);

#endif /* XE_WL_BRIDGE_H */
