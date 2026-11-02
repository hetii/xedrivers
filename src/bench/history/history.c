/* A flight recorder: every HISTORY_EVERY worker ticks (200 ms) the counters that
 * show whether frames still move each way, in a ring of HISTORY samples (about
 * 100 s), so that a transfer that stalls on a link the PC cannot reach -- a
 * phone's network -- can be read back afterwards over the onboard port
 * (xectl.py history). It found the RNDIS upload that crawled because the phone
 * held received frames back for blocks that never filled.
 */

#include "history.h"
#include "net.h"
#include "xnet.h"

#define HISTORY       512
#define HISTORY_EVERY 10

/* Field order is tools/xectl.py's history(). */
typedef struct {
    u32 tick;
    u32 rx_frames;
    u32 tx_frames;
    u32 tx_inflight;
    u32 tx_drops;
    u32 rx_errors;
    u32 xmit_usb;           /* frames xnet handed us */
    u32 xmit_dropped;       /* frames xnet's queue gave up on */
    u32 rx_bytes;           /* low words: a sample's difference is exact */
    u32 tx_bytes;           /* what the USB OUT endpoint took, headers included */
} XE_SAMPLE;

volatile XE_SAMPLE xe_history[HISTORY];
volatile u32 xe_history_count;          /* samples written, ever */

void history_tick(u32 tick)
{
    volatile XE_SAMPLE *at;
    u32 i;

    if (tick % HISTORY_EVERY)
        return;
    at = &xe_history[xe_history_count % HISTORY];
    at->tick = tick;
    at->rx_frames = at->tx_frames = at->tx_inflight = at->tx_drops = 0;
    at->rx_errors = at->rx_bytes = at->tx_bytes = 0;
    for (i = 0; i < NET_MAX; i++) {             /* every device, summed */
        at->rx_frames += net_stats[i].rx_frames;
        at->tx_frames += net_stats[i].tx_frames;
        at->tx_inflight += net_stats[i].tx_inflight;
        at->tx_drops += net_stats[i].tx_drops;
        at->rx_errors += net_stats[i].rx_errors;
        at->rx_bytes += net_stats[i].rx_bytes;
        at->tx_bytes += net_stats[i].tx_bytes;
    }
    at->xmit_usb = xnet_stats.xmit_usb;
    at->xmit_dropped = xnet_stats.xmit_dropped;
    mem_sync();
    xe_history_count = xe_history_count + 1;
}
