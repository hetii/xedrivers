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

static void tx_done(URB *urb, u32 status)
{
    RTW *r = (RTW *)urb->Owner;

    if (!NT_SUCCESS(status)) {
        r->st->tx_errors++;
        r->st->tx_last_error = status;
    } else {
        r->st->tx_done++;
    }
    mem_sync();
    r->tx_busy[urb->Tag] = 0;
    __sync_fetch_and_sub(&r->tx_inflight, 1);
    if (r->nd)
        net_tx_done(r->nd);             /* what waited for a slot goes now */
}

static u32 take_slot(RTW *r)
{
    u32 i, n;

    for (n = 0; n < RTW_TX_COUNT; n++) {
        i = __sync_fetch_and_add(&r->tx_next, 1) % RTW_TX_COUNT;
        if (!__sync_lock_test_and_set(&r->tx_busy[i], 1))
            return i;
    }
    return RTW_TX_COUNT;
}

u32 rtw_tx_space(RTW *r)
{
    return r->tx_inflight < RTW_TX_COUNT - 1;
}

/* One 802.11 frame in two parts (softmac.h's tx). */
u32 rtw_tx(RTW *r, const u8 *head, u32 head_length, const u8 *body, u32 body_length,
           u32 flags, u32 tid)
{
    u32 slot, i, qsel, ep, bmc = flags & RTW_TX_GROUP ? 1 : 0, seq,
        length = head_length + body_length;
    const u8 *frame = head;
    u8 *b;
    u16 sum = 0;

    if (!r->tx_ready || length + RTW_TX_DESC_SIZE > RTW_TX_SLOT) {
        r->st->tx_drops++;
        return 0;
    }
    slot = take_slot(r);
    if (slot == RTW_TX_COUNT) {
        r->st->tx_full++;
        return 0;
    }
    b = r->tx_buf[slot];
    if (!b) {
        r->tx_busy[slot] = 0;
        r->st->tx_drops++;
        return 0;
    }
    for (i = 0; i < RTW_TX_DESC_SIZE; i++)
        b[i] = 0;

    if (flags & RTW_TX_MGMT) {
        /* rtw_tx_mgmt_pkt_info_update with rtw_tx_pkt_info_update_rate */
        qsel = QSEL_MGMT;
        ep = RTW_EP_HIGH;
        put32(b + 4, (u32)r->port_macid | qsel << 8 |
                     (r->channel <= 14 ? RATEID_B_20M : RATEID_G) << 16);
        put32(b + 12, 1u << 8 | 1u << 10);             /* USE_RATE, DISDATAFB */
        put32(b + 16, r->channel <= 14 ? DESC_RATE1M : DESC_RATE6M);
        put32(b + 32, 1u << 15);                        /* EN_HWSEQ, hw_ssn_sel 0 */
        put32(b, length | RTW_TX_DESC_SIZE << 16 | bmc << 24 | 1u << 26 | 1u << 31);
    } else {
        /* rtw_tx_data_pkt_info_update, a rate the link picked (no adaptation yet),
         * and rtw_tx_pkt_info_update_sec */
        u32 rate = flags & RTW_TX_EAPOL ? (r->channel <= 14 ? DESC_RATE1M : DESC_RATE6M)
                                        : r->data_rate;

        qsel = tid & 7;
        ep = qsel == 4 || qsel == 5 || qsel == 6 || qsel == 7 ? RTW_EP_NORMAL : RTW_EP_LOW;
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
        seq = (u32)(frame[22] | frame[23] << 8) >> 4;
        put32(b + 36, seq << 12);                       /* SW_SEQ */
        put32(b, length | RTW_TX_DESC_SIZE << 16 | bmc << 24 | 1u << 26);
    }
    for (i = 0; i < 32; i += 2)                         /* fill_txdesc_checksum_common */
        sum ^= (u16)(b[i] | b[i + 1] << 8);
    b[28] = (u8)sum;
    b[29] = (u8)(sum >> 8);
    for (i = 0; i < head_length; i++)
        b[RTW_TX_DESC_SIZE + i] = head[i];
    for (i = 0; i < body_length; i++)
        b[RTW_TX_DESC_SIZE + head_length + i] = body[i];

    /* rtw88 sends with URB_ZERO_PACKET, so a transfer of whole 512-byte packets
     * still ends; here one byte more past the frame does it, as send_firmware_pkt
     * does for the firmware (the descriptor's size says where the frame ends). */
    i = RTW_TX_DESC_SIZE + length;
    if (!(i & 511)) {
        b[i++] = 0;
        r->st->tx_padded++;
    }
    __sync_fetch_and_add(&r->tx_inflight, 1);
    r->st->tx_frames++;
    usb_prepare(&r->tx_slot_urb[slot], r->dev, r->ep_out[ep], tx_done, b, i, 0);
    r->tx_slot_urb[slot].Owner = r;
    r->tx_slot_urb[slot].Tag = slot;
    usb_submit(&r->tx_slot_urb[slot]);
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
