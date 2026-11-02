/* The RTL8822BU's transmit path and keys, as rtw88's tx.c, usb.c and sec.c do them.
 *
 * A frame goes out behind a 48-byte descriptor (rtw_tx_fill_tx_desc), one frame to
 * a bulk transfer, on the endpoint its queue maps to: management on the high
 * priority one (0x05), data by its TID -- best effort and background on the low
 * (0x08), video and voice on the normal (0x06). Management frames go at the band's
 * lowest rate with the hardware numbering them; data frames carry the station
 * layer's sequence number, and with a key the hardware encrypts them and adds the
 * MIC (the CCMP header is the layer's: rtw88 asks mac80211 to generate the IV).
 */

#include "rtw.h"
#include "prof.h"
#include "bench.h"

#define DESC_RATE1M         0x00
#define DESC_RATE6M         0x04
#define DESC_RATE54M        0x0B
#define RATEID_G            7           /* RTW_RATEID_G */
#define RATEID_B_20M        8
#define QSEL_MGMT           18
#define MACID_AP            1           /* rtw88 gives the AP the first after the port's */
#define SEC_TYPE_CCMP       3

#define SEC_CMD_REG         0x670
#define SEC_WRITE_REG       0x674
#define SEC_CMD_WRITE       BIT(16)
#define SEC_CMD_POLLING     BIT(31)
#define SEC_CMD_CLEAR       BIT(30)
#define CAM_AES             4
#define CAM_PAIRWISE_SLOT   4           /* after the four default (group) keys */

static void put32(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

#define FRAME_MAX       (2048 - RTW_TX_DESC_SIZE)  /* a descriptor and the frame, one at a time */
#define EP_DIRECT       1           /* transfers out on an endpoint before frames join: 1
                                     * against 4 put the console's ACKs in fewer
                                     * transfers, 8 streams in 27.8 -> 28.5 MB/s */
#define EP_KEEP_OUT     4           /* a completion sends the open aggregate only below
                                     * this many out. Sending it at every completion
                                     * kept ~14 transfers out of ~1.8 frames each under
                                     * load (7540 a second); now they leave full, 16
                                     * frames (915 a second): 8 streams out 156 -> 170
                                     * Mbit/s, 1, 2, 4 and 8 alike; one stream out, in,
                                     * and FTP unchanged (2026-11-03, 5 GHz ch 116) */

static u32 align8(u32 n)
{
    return (n + 7) & ~7u;
}

/* rtw_usb_fill_tx_checksum: the first descriptor says how many follow it, and its
 * checksum covers that; a transfer of whole 512-byte packets gets one byte more,
 * the zero-length packet rtw88 asks for (URB_ZERO_PACKET) the descriptors' sizes
 * already mark the end. Returns the transfer's length. */
static u32 seal(RTW *r, u8 *b, u32 length, u32 frames)
{
    u32 i;
    u16 sum = 0;

    if (frames > 1) {
        b[31] = (u8)frames;                         /* w7 DMA_TXAGG_NUM */
        b[28] = b[29] = 0;
        for (i = 0; i < 32; i += 2)
            sum ^= (u16)(b[i] | b[i + 1] << 8);
        b[28] = (u8)sum;
        b[29] = (u8)(sum >> 8);
        r->st->tx_aggregated++;
    }
    if (!(length & 511))
        b[length++] = 0;
    return length;
}

static void tx_done(URB *urb, u32 status);

static void submit(RTW *r, u32 slot, u32 length)
{
    r->st->tx_transfers++;
    PROF_MARK(r->tx_slot_urb[slot].Stamp);
    usb_prepare(&r->tx_slot_urb[slot], r->dev, r->ep_out[r->tx_slot_ep[slot]], tx_done,
                r->tx_buf[slot], length, 0);
    r->tx_slot_urb[slot].Owner = r;
    r->tx_slot_urb[slot].Tag = slot;
    usb_submit(&r->tx_slot_urb[slot]);
}

/* Under tx_lock: the endpoint's open aggregate, sealed and closed; its slot, or
 * RTW_TX_COUNT when there was none. */
static u32 close_open(RTW *r, u32 ep, u32 *length)
{
    u32 slot = r->tx_open[ep];

    if (slot == RTW_TX_COUNT)
        return slot;
    *length = seal(r, r->tx_buf[slot], r->tx_open_length[ep], r->tx_open_frames[ep]);
    r->tx_open[ep] = RTW_TX_COUNT;
    r->tx_ep_busy[ep]++;
    return slot;
}

static void tx_done(URB *urb, u32 status)
{
    RTW *r = (RTW *)urb->Owner;
    u32 slot = urb->Tag, ep = r->tx_slot_ep[slot], irql, next, length = 0;
    PROF_START(t);

    PROF_SINCE(tx_wait, urb->Stamp, urb->Length);
    if (!NT_SUCCESS(status)) {
        r->st->tx_errors++;
        r->st->tx_last_error = status;
    } else {
        r->st->tx_done++;
    }
    /* what joined while this endpoint was busy goes now, unless EP_KEEP_OUT are
     * still out (then it fills on, and goes when full or when fewer are out) */
    irql = KfAcquireSpinLock(&r->tx_lock);
    r->tx_ep_busy[ep]--;
    next = r->tx_ep_busy[ep] < EP_KEEP_OUT ? close_open(r, ep, &length) : RTW_TX_COUNT;
    KfReleaseSpinLock(&r->tx_lock, irql);
    if (next != RTW_TX_COUNT)
        submit(r, next, length);

    mem_sync();
    r->tx_busy[slot] = 0;
    __sync_fetch_and_sub(&r->tx_inflight, 1);
    if (r->nd)
        net_tx_done(r->nd);             /* what waited for a slot goes now */
    PROF_END(tx_done, t, urb->Length);
}

static u32 take_slot(RTW *r)
{
    u32 i, n;

    for (n = 0; n < RTW_TX_COUNT; n++) {
        i = __sync_fetch_and_add(&r->tx_next, 1) % RTW_TX_COUNT;
        if (!__sync_lock_test_and_set(&r->tx_busy[i], 1)) {
            __sync_fetch_and_add(&r->tx_inflight, 1);
            return i;
        }
    }
    return RTW_TX_COUNT;
}

u32 rtw_tx_space(RTW *r)
{
    return r->tx_inflight < RTW_TX_COUNT - 1;
}

/* One frame's descriptor and the frame, at `b`. */
static void fill(RTW *r, u8 *b, const u8 *head, u32 head_length, const u8 *body,
                 u32 body_length, u32 flags, u32 tid)
{
    u32 i, qsel, bmc = flags & RTW_TX_GROUP ? 1 : 0, seq, length = head_length + body_length;
    u16 sum = 0;

    for (i = 0; i < RTW_TX_DESC_SIZE; i++)
        b[i] = 0;
    if (flags & RTW_TX_MGMT) {
        /* rtw_tx_mgmt_pkt_info_update with rtw_tx_pkt_info_update_rate */
        qsel = QSEL_MGMT;
        put32(b + 4, (u32)r->port_macid | qsel << 8 |
                     (r->channel <= 14 ? RATEID_B_20M : RATEID_G) << 16);
        put32(b + 12, 1u << 8 | 1u << 10);             /* USE_RATE, DISDATAFB */
        put32(b + 16, r->channel <= 14 ? DESC_RATE1M : DESC_RATE6M);
        put32(b + 32, 1u << 15);                        /* EN_HWSEQ, hw_ssn_sel 0 */
        put32(b, length | RTW_TX_DESC_SIZE << 16 | bmc << 24 | 1u << 26 | 1u << 31);
    } else {
        /* rtw_tx_data_pkt_info_update and rtw_tx_pkt_info_update_sec: EAPOL at the
         * band's lowest rate, the rest at the firmware's (or the link's fixed one) */
        u32 rate = flags & RTW_TX_EAPOL ? (r->channel <= 14 ? DESC_RATE1M : DESC_RATE6M)
                                        : r->data_rate;

        qsel = tid & 7;
        put32(b + 4, (bmc ? (u32)r->port_macid : MACID_AP) | qsel << 8 |
                     r->data_rate_id << 16 |
                     (flags & RTW_TX_PROTECT ? SEC_TYPE_CCMP << 22 : 0));
        if (flags & RTW_TX_EAPOL) {
            put32(b + 12, 1u << 8 | 1u << 10);          /* USE_RATE, DISDATAFB */
        } else {
            if (r->ampdu)                               /* AGG_EN, AMPDU_DEN, MAX_AGG_NUM */
                put32(b + 8, 1u << 12 | (r->ampdu_density & 7) << 20);
            put32(b + 12, (r->data_use_rate ? 1u << 8 : 0) |
                          (r->ampdu ? (r->ampdu_factor & 0x1F) << 17 : 0));
            put32(b + 20, r->data_bw << 5 | (r->data_sgi ? 1u << 4 : 0) |
                          (r->data_ldpc ? 1u << 7 : 0) | (r->data_stbc ? 1u << 8 : 0));
        }
        put32(b + 16, rate);                            /* DATARATE */
        seq = (u32)(head[22] | head[23] << 8) >> 4;
        put32(b + 36, seq << 12);                       /* SW_SEQ */
        put32(b, length | RTW_TX_DESC_SIZE << 16 | bmc << 24 | 1u << 26);
    }
    for (i = 0; i < 32; i += 2)                         /* fill_txdesc_checksum_common */
        sum ^= (u16)(b[i] | b[i + 1] << 8);
    b[28] = (u8)sum;
    b[29] = (u8)(sum >> 8);
    xe_copy(b + RTW_TX_DESC_SIZE, head, head_length);
    xe_copy(b + RTW_TX_DESC_SIZE + head_length, body, body_length);
}

static u32 endpoint_of(u32 flags, u32 tid)
{
    if (flags & RTW_TX_MGMT)
        return RTW_EP_HIGH;
    tid &= 7;
    return tid >= 4 ? RTW_EP_NORMAL : RTW_EP_LOW;
}

/* One 802.11 frame in two parts (softmac.h's tx). While an endpoint has EP_DIRECT
 * transfers out, frames join one open transfer there (rtw_usb_tx_agg_skb), up to
 * RTW_TX_AGG, each behind its own descriptor at a multiple of 8; the transfer goes
 * when it is full or when one of that endpoint's completes. Any IRQL. */
u32 rtw_tx(RTW *r, const u8 *head, u32 head_length, const u8 *body, u32 body_length,
           u32 flags, u32 tid)
{
    u32 length = head_length + body_length, ep = endpoint_of(flags, tid), irql, at, slot,
        full = RTW_TX_COUNT, full_length = 0, now = RTW_TX_COUNT, now_length = 0;

    if (!r->tx_ready || length > FRAME_MAX) {
        r->st->tx_drops++;
        return 0;
    }
    irql = KfAcquireSpinLock(&r->tx_lock);
    slot = r->tx_open[ep];
    if (slot != RTW_TX_COUNT &&
        align8(r->tx_open_length[ep]) + RTW_TX_DESC_SIZE + length > RTW_TX_SLOT - 8)
        full = close_open(r, ep, &full_length);         /* no room: it goes as it is */
    slot = r->tx_open[ep];
    if (slot == RTW_TX_COUNT) {
        slot = take_slot(r);
        if (slot == RTW_TX_COUNT) {
            KfReleaseSpinLock(&r->tx_lock, irql);
            if (full != RTW_TX_COUNT)
                submit(r, full, full_length);
            r->st->tx_full++;
            return 0;
        }
        r->tx_slot_ep[slot] = ep;
        if (r->tx_ep_busy[ep] < EP_DIRECT) {
            /* the endpoint keeps up: this frame alone, now */
            PROF_START(c);
            fill(r, r->tx_buf[slot], head, head_length, body, body_length, flags, tid);
            PROF_END(copy, c, length);
            now = slot;
            now_length = seal(r, r->tx_buf[slot], RTW_TX_DESC_SIZE + length, 1);
            r->tx_ep_busy[ep]++;
            goto out;
        }
        r->tx_open[ep] = slot;
        r->tx_open_length[ep] = 0;
        r->tx_open_frames[ep] = 0;
    }
    at = align8(r->tx_open_length[ep]);
    for (; r->tx_open_length[ep] < at; r->tx_open_length[ep]++)
        r->tx_buf[slot][r->tx_open_length[ep]] = 0;
    {
        PROF_START(c);
        fill(r, r->tx_buf[slot] + at, head, head_length, body, body_length, flags, tid);
        PROF_END(copy, c, length);
    }
    r->tx_open_length[ep] = at + RTW_TX_DESC_SIZE + length;
    if (++r->tx_open_frames[ep] == RTW_TX_AGG)
        now = close_open(r, ep, &now_length);
out:
    r->st->tx_frames++;
    KfReleaseSpinLock(&r->tx_lock, irql);
    if (full != RTW_TX_COUNT)
        submit(r, full, full_length);
    if (now != RTW_TX_COUNT)
        submit(r, now, now_length);
    return 1;
}

/* --- keys: rtw_sec_write_cam --------------------------------------------------- */

static void cam_write(RTW *r, u32 slot, u32 keyidx, u32 group, const u8 *addr, const u8 *key)
{
    u32 i, content, base = slot << 3;

    for (i = 8; i-- > 0;) {
        if (i == 0)
            content = (keyidx & 3) | CAM_AES << 2 | group << 6 | 1u << 15 |
                      (u32)addr[0] << 16 | (u32)addr[1] << 24;
        else if (i == 1)
            content = addr[2] | (u32)addr[3] << 8 | (u32)addr[4] << 16 | (u32)addr[5] << 24;
        else if (i >= 6)
            content = 0;
        else
            content = key[(i - 2) * 4] | (u32)key[(i - 2) * 4 + 1] << 8 |
                      (u32)key[(i - 2) * 4 + 2] << 16 | (u32)key[(i - 2) * 4 + 3] << 24;
        rtw_write32(r, SEC_WRITE_REG, content);
        rtw_write32(r, SEC_CMD_REG, SEC_CMD_WRITE | SEC_CMD_POLLING | (base + i));
    }
}

/* softmac.h's set_key: a CCMP key, the pairwise one under the AP's address in the
 * first slot after the defaults, a group one in the slot of its index. */
u32 rtw_set_key(RTW *r, u32 pairwise, u32 index, const u8 *key, u32 length, const u8 *peer)
{
    static const u8 broadcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

    if (length != 16)
        return 0;
    if (pairwise)
        cam_write(r, CAM_PAIRWISE_SLOT, 0, 0, peer, key);
    else
        cam_write(r, index & 3, index & 3, 1, broadcast, key);
    r->st->keys++;
    return 1;
}

/* rtw_sec_clear_cam for every slot in use. */
void rtw_clear_keys(RTW *r)
{
    u32 slot;

    for (slot = 0; slot <= CAM_PAIRWISE_SLOT; slot++) {
        rtw_write32(r, SEC_WRITE_REG, 0);
        rtw_write32(r, SEC_CMD_REG, SEC_CMD_WRITE | SEC_CMD_POLLING | (slot << 3));
    }
}
