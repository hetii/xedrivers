/* The SoftMAC station's data path: Ethernet frames to 802.11 and back.
 *
 * Out: a QoS data frame to the AP (ToDS), the destination as the third address,
 * LLC/SNAP and the EtherType before the payload, and with the pairwise key the
 * CCMP header whose packet number the card cannot make itself (rtw88 asks for a
 * software IV); the card encrypts and adds the MIC.
 * In: frames from the AP (FromDS) to us or a group, their CCMP header and MIC
 * left in by the card taken off, A-MSDU subframes split, EAPOL to the handshake
 * (wpa.c), the rest to the network layer as Ethernet.
 */

#include "station.h"
#include "wpa.h"
#include "bench.h"

#define ETHERTYPE_EAPOL 0x888Eu
#define HDR_LEN         24
#define QOS_LEN         2
#define CCMP_HDR_LEN    8
#define CCMP_MIC_LEN    8
#define SNAP_LEN        8               /* AA AA 03 00 00 00 and the EtherType */
#define FC_DATA_QOS     0x88
#define FC_TODS         0x01
#define FC_FROMDS       0x02
#define FC_PROTECTED    0x40
#define FC_ORDER        0x80
#define FC_RETRY        0x08
#define ETH_MAX         1514

static u32 same(const u8 *a, const u8 *b)
{
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3] && a[4] == b[4] &&
           a[5] == b[5];
}

/* Build and send one data frame: `type` and `payload` are what follows the
 * Ethernet header. The headers are built here, the payload goes as it is. */
static u32 send_data(WL_STA *s, const u8 *da, u32 type, const u8 *payload, u32 length,
                     u32 eapol)
{
    u8 h[HDR_LEN + QOS_LEN + CCMP_HDR_LEN + SNAP_LEN];
    u32 n, i, protect = s->pairwise_set, flags = eapol ? WL_TX_EAPOL : 0, seq, lo, hi;

    if (length > ETH_MAX - 14)
        return 0;
    h[0] = FC_DATA_QOS;
    h[1] = (u8)(FC_TODS | (protect ? FC_PROTECTED : 0));
    h[2] = h[3] = 0;
    for (i = 0; i < 6; i++) {
        h[4 + i] = s->bss.bssid[i];
        h[10 + i] = s->own[i];
        h[16 + i] = da[i];
    }
    seq = __sync_fetch_and_add(&s->seq, 1) & 0xFFF;
    h[22] = (u8)(seq << 4);
    h[23] = (u8)(seq >> 4);
    h[24] = 0;                              /* QoS: TID 0, normal ack */
    h[25] = 0;
    n = HDR_LEN + QOS_LEN;
    if (protect) {
        lo = __sync_add_and_fetch(&s->pn_lo, 1);
        if (!lo)
            __sync_fetch_and_add(&s->pn_hi, 1);
        hi = s->pn_hi;
        h[n++] = (u8)lo;
        h[n++] = (u8)(lo >> 8);
        h[n++] = 0;
        h[n++] = 0x20;                      /* ExtIV, key 0 */
        h[n++] = (u8)(lo >> 16);
        h[n++] = (u8)(lo >> 24);
        h[n++] = (u8)hi;
        h[n++] = (u8)(hi >> 8);
        flags |= WL_TX_PROTECT;
    }
    h[n++] = 0xAA;                          /* LLC/SNAP */
    h[n++] = 0xAA;
    h[n++] = 0x03;
    h[n++] = 0;
    h[n++] = 0;
    h[n++] = 0;
    h[n++] = (u8)(type >> 8);
    h[n++] = (u8)type;
    if (!s->hw->tx(s->hw->context, h, n, payload, length, flags, 0)) {
        s->st->tx_dropped++;
        return 0;
    }
    s->st->tx_data++;
    return 1;
}

u32 wl_sta_xmit(WL_STA *s, const u8 *eth, u32 length)
{
    if (s->state != WL_STA_JOINED || !s->port_open || length < 14) {
        s->st->tx_dropped++;
        return 0;
    }
    return send_data(s, eth, (u32)eth[12] << 8 | eth[13], eth + 14, length - 14, 0);
}

/* wpa.c's WPA_HOST send: one EAPOL frame to the AP, before or after the keys. */
u32 wl_sta_send_eapol(void *context, const u8 *body, u32 length)
{
    WL_STA *s = (WL_STA *)context;

    return send_data(s, s->bss.bssid, ETHERTYPE_EAPOL, body, length, 1);
}

/* One MSDU: its LLC/SNAP and EtherType at `p`, then the payload, between `da` and
 * `sa`. The Ethernet header is written in place over the six bytes before the
 * EtherType, which belong to headers already read: the receive buffer is the
 * card's, and nothing is copied. */
static void deliver(WL_STA *s, const u8 *da_in, const u8 *sa_in, u8 *p, u32 length,
                    u32 tid, u32 seq)
{
    u8 da[6], sa[6], *eth;
    u32 type, i;

    if (length < SNAP_LEN || p[0] != 0xAA || p[1] != 0xAA || p[2] != 0x03 ||
        length - SNAP_LEN > ETH_MAX - 14) {
        s->st->rx_dropped++;
        return;
    }
    type = (u32)p[6] << 8 | p[7];
    if (type == ETHERTYPE_EAPOL) {
        s->st->rx_eapol++;
        wpa_eapol_in(p + SNAP_LEN, length - SNAP_LEN);
        return;
    }
    if (!s->port_open) {
        s->st->rx_dropped++;
        return;
    }
    for (i = 0; i < 6; i++) {
        da[i] = da_in[i];
        sa[i] = sa_in[i];
    }
    eth = p + SNAP_LEN - 14;
    for (i = 0; i < 6; i++) {
        eth[i] = da[i];
        eth[6 + i] = sa[i];
    }
    s->st->rx_data++;
    /* a group frame is numbered apart from the session's and is not reordered */
    if (tid < WL_REORDER_TIDS && (s->rx_ba >> tid) & 1 && !(da_in[0] & 1)) {
        bench_wl_frame(BENCH_WL_IN, seq, eth, 14 + length - SNAP_LEN);
        wl_reorder_in(&s->reorder, tid, seq, eth, 14 + length - SNAP_LEN);
    } else {
        net_frame_in(s->nd, eth, 14 + length - SNAP_LEN);
    }
}

void wl_sta_rx_data(WL_STA *s, u8 *f, u32 length, u32 decrypted)
{
    u32 hdr = HDR_LEN, amsdu = 0, at, n, tid = WL_REORDER_TIDS;
    u32 seq = (u32)(f[22] | f[23] << 8) >> 4;
    const u8 *da = f + 4, *sa = f + 16;

    if ((f[1] & (FC_TODS | FC_FROMDS)) != FC_FROMDS || !same(f + 10, s->bss.bssid) ||
        (!same(da, s->own) && !(da[0] & 1))) {
        s->st->rx_dropped++;
        return;
    }
    if (f[0] & 0x80) {                      /* QoS subtypes */
        if (f[0] & 0x40)                    /* QoS null: nothing in it */
            return;
        amsdu = f[24] & 0x80;
        tid = f[24] & 7;
        hdr += QOS_LEN;
        if (f[1] & FC_ORDER)
            hdr += 4;                       /* HT control */
    } else if (f[0] & 0x40) {
        return;                             /* null */
    }
    /* A retransmission of the last frame of its TID: the first one made it, only
     * the acknowledgement was lost (ieee80211_rx_h_check_dup, unicast only). */
    if (!(da[0] & 1)) {
        u32 i = tid < WL_REORDER_TIDS ? tid : WL_REORDER_TIDS, sc = f[22] | f[23] << 8;

        if ((f[1] & FC_RETRY) && s->last_seq_ctrl[i] == sc) {
            bench_wl_frame(BENCH_WL_DUP, sc >> 4, 0, 0);
            s->st->rx_duplicates++;
            return;
        }
        s->last_seq_ctrl[i] = (u16)sc;
    }
    if (f[1] & FC_PROTECTED) {
        if (!decrypted || length < hdr + CCMP_HDR_LEN + CCMP_MIC_LEN) {
            s->st->rx_dropped++;
            return;
        }
        hdr += CCMP_HDR_LEN;
        length -= CCMP_MIC_LEN;
    }
    if (length <= hdr) {
        s->st->rx_dropped++;
        return;
    }
    if (!amsdu) {
        deliver(s, da, sa, f + hdr, length - hdr, tid, seq);
        return;
    }
    /* A-MSDU: subframes of DA, SA, length, the MSDU, padded to four bytes */
    for (at = hdr; at + 14 <= length; at += (14 + n + 3) & ~3u) {
        n = (u32)f[at + 12] << 8 | f[at + 13];
        if (at + 14 + n > length)
            break;
        deliver(s, f + at, f + at + 6, f + at + 14, n, WL_REORDER_TIDS, 0);
    }
}
