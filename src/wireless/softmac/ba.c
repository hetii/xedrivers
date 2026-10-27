/* The SoftMAC station's block-ack sessions: what lets the AP and the station put
 * many frames into one A-MPDU, which FullMAC firmware negotiates by itself.
 *
 * The AP asks (ADDBA request) for each TID it sends on; the answer accepts 64
 * frames with immediate block ack and no A-MSDU in them (reorder.c orders single
 * frames by sequence number), the card acknowledges the aggregates by itself, and
 * that TID's frames go through reorder.c, whose timeouts run in the receive path's
 * context through a DPC. The station asks the AP the same for TID 0 once the port
 * is open, and on yes the card aggregates what it sends (set_aggregation).
 */

#include "station.h"
#include "frames.h"
#include "usb.h"

#define CATEGORY_BLOCK_ACK  3
#define ADDBA_REQUEST       0
#define ADDBA_RESPONSE      1
#define DELBA               2
#define BA_POLICY_IMMEDIATE 0x0002u
#define BA_BUFFER_MAX       64

static u32 le16(const u8 *p)
{
    return (u32)p[0] | (u32)p[1] << 8;
}

/* One action frame from the AP (management subtype 13). DISPATCH_LEVEL. */
void wl_sta_action(WL_STA *s, const u8 *f, u32 length)
{
    u8 resp[WL_FRAME_MAX];
    u32 param, tid, size;

    if (length < 27 || f[24] != CATEGORY_BLOCK_ACK)
        return;
    switch (f[25]) {
    case ADDBA_REQUEST:
        if (length < 33)
            return;
        param = le16(f + 27);
        tid = (param >> 2) & 15;
        size = param >> 6;
        if (!size || size > BA_BUFFER_MAX)
            size = BA_BUFFER_MAX;
        s->st->addba_requests++;
        param = BA_POLICY_IMMEDIATE | (tid & 15) << 2 | size << 6;     /* no A-MSDU */
        s->hw->tx(s->hw->context, resp,
                  wl_frame_addba_resp(resp, s->own, s->bss.bssid, f[26], 0, param,
                                      le16(f + 29)),
                  0, 0, WL_TX_MGMT, 0);
        if (tid < WL_REORDER_TIDS) {
            __sync_fetch_and_or(&s->rx_ba, 1u << tid);
            s->st->addba_accepted++;
            s->st->rx_ba = s->rx_ba;
        }
        break;
    case ADDBA_RESPONSE:
        if (length >= 29 && f[26] == (u8)s->ba_token && s->tx_ba == 1) {
            s->st->tx_ba = le16(f + 27) == 0 ? 2 : 3;
            if (le16(f + 27) == 0) {
                s->tx_ba = 2;
                s->hw->set_aggregation(s->hw->context, 0, 1);
            } else {
                s->tx_ba = 3;                   /* refused: not asked again */
            }
        }
        break;
    case DELBA:
        if (length < 28)
            return;
        param = le16(f + 26);
        tid = param >> 12;
        if (param & 0x0800) {                   /* the AP's own session to us */
            __sync_fetch_and_and(&s->rx_ba, ~(1u << tid));
            if (tid < WL_REORDER_TIDS)
                wl_reorder_end(&s->reorder, tid);
        } else if (tid == 0) {
            s->tx_ba = 0;
            s->hw->set_aggregation(s->hw->context, 0, 0);
        }
        break;
    default:
        break;
    }
}

static void reorder_dpc(KDPC *dpc, void *context, void *a1, void *a2)
{
    WL_STA *s = (WL_STA *)context;

    (void)dpc; (void)a1; (void)a2;
    s->reorder_dpc_queued = 0;
    if (s->state == WL_STA_JOINED)
        wl_reorder_expire(&s->reorder);
}

void wl_sta_ba_init(WL_STA *s)
{
    KeInitializeDpc(&s->reorder_dpc, reorder_dpc, s);
    s->reorder_dpc.TargetNumber = USB_PROCESSOR + 1;    /* the receive path's processor */
    s->reorder_dpc_queued = 0;
}

/* A new link: no sessions yet. */
void wl_sta_ba_reset(WL_STA *s)
{
    s->rx_ba = 0;
    if (s->tx_ba == 2)
        s->hw->set_aggregation(s->hw->context, 0, 0);
    s->tx_ba = 0;
    wl_reorder_reset(&s->reorder);
}

/* Every worker tick while joined: our own session once the port is open, and the
 * reorder windows' timeouts. */
void wl_sta_ba_tick(WL_STA *s)
{
    u8 f[WL_FRAME_MAX];

    if (s->port_open && s->tx_ba == 0 && s->st->tx_data > 16) {
        s->tx_ba = 1;
        s->ba_token = (s->ba_token + 1) & 0xFF;
        s->hw->tx(s->hw->context, f,
                  wl_frame_addba_req(f, s->own, s->bss.bssid, s->ba_token, 0, s->seq),
                  0, 0, WL_TX_MGMT, 0);
    }
    if (s->rx_ba && wl_reorder_tick(&s->reorder) &&
        !__sync_lock_test_and_set(&s->reorder_dpc_queued, 1))
        KeInsertQueueDpc(&s->reorder_dpc, 0, 0);
}
