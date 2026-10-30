/* The frames of joining a network and their elements, built and read here for any
 * SoftMAC card (softmac.h): probe request, authentication, association request,
 * deauthentication; the AP's elements turned into the link's description.
 *
 * What the station announces is a 2x2 802.11ac station without beamforming: rtw88's
 * rtw_init_ht_cap and rtw_init_vht_cap for the 8822B, the beamformee bits left out
 * because nothing here answers a sounding.
 */

#include "frames.h"

#define IE_SSID         0
#define IE_RATES        1
#define IE_DS           3
#define IE_HT_CAP       45
#define IE_RSN          48
#define IE_EXT_RATES    50
#define IE_HT_OP        61
#define IE_VHT_CAP      191
#define IE_VHT_OP       192
#define IE_VENDOR       221

#define HT_CAP_INFO     0x196Fu     /* LDPC, 20/40, SM power save disabled, SGI 20
                                     * and 40, RX STBC 1, 7935-byte A-MSDU, DSSS/CCK
                                     * in 40. SM power save is bits 2-3; 0 there,
                                     * "static", had the AP send on one stream only
                                     * (VHT NSS 1, 433 Mbit/s); 3, disabled, as
                                     * mac80211 sends, gives two (NSS 2, 867): on a
                                     * clean channel 8 streams in 218 -> 222 Mbit/s,
                                     * 4 streams 200 -> 205. */
#define HT_AMPDU_PARAMS 0x13u       /* 64 KB, 2 us spacing */
#define VHT_CAP_INFO    0x03C001B2u /* 11454-byte MPDU, RX LDPC, SGI 80, TX STBC,
                                     * RX STBC 1, +HTC-VHT, A-MPDU exponent 7 */
#define VHT_MCS_MAP     0xFFFAu     /* MCS 0-9 on two streams */

static u32 put(u8 *p, const u8 *from, u32 n)
{
    u32 i;

    for (i = 0; i < n; i++)
        p[i] = from[i];
    return n;
}

static void put16(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}

static void put32(u8 *p, u32 v)
{
    put16(p, v);
    put16(p + 2, v >> 16);
}

static u32 le16(const u8 *p)
{
    return (u32)p[0] | (u32)p[1] << 8;
}

/* The 24-byte header of a management frame. */
static u32 header(u8 *f, u32 subtype, const u8 *da, const u8 *sa, const u8 *bssid, u32 seq)
{
    f[0] = (u8)(subtype << 4);
    f[1] = 0;
    f[2] = f[3] = 0;                        /* duration: the hardware's */
    put(f + 4, da, 6);
    put(f + 10, sa, 6);
    put(f + 16, bssid, 6);
    put16(f + 22, (seq & 0xFFF) << 4);
    return 24;
}

static u32 ie(u8 *p, u32 id, const u8 *body, u32 n)
{
    p[0] = (u8)id;
    p[1] = (u8)n;
    return 2 + put(p + 2, body, n);
}

/* Our rates, in 500 kb/s units: 2.4 GHz has CCK first. */
static u32 rates(u8 *p, u32 channel)
{
    static const u8 g[] = { 0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24 };
    static const u8 g_ext[] = { 0x30, 0x48, 0x60, 0x6C };
    static const u8 a[] = { 0x8C, 0x12, 0x98, 0x24, 0xB0, 0x48, 0x60, 0x6C };
    u32 n;

    if (channel > 14)
        return ie(p, IE_RATES, a, sizeof(a));
    n = ie(p, IE_RATES, g, sizeof(g));
    return n + ie(p + n, IE_EXT_RATES, g_ext, sizeof(g_ext));
}

static u32 ht_cap(u8 *p)
{
    u8 b[26];
    u32 i;

    for (i = 0; i < sizeof(b); i++)
        b[i] = 0;
    put16(b, HT_CAP_INFO);
    b[2] = HT_AMPDU_PARAMS;
    b[3] = 0xFF;                            /* MCS 0-7 */
    b[4] = 0xFF;                            /* MCS 8-15 */
    b[7] = 0x01;                            /* MCS 32 */
    put16(b + 13, 300);                     /* highest RX rate, Mb/s */
    b[15] = 0x01;                           /* TX MCS set defined */
    return ie(p, IE_HT_CAP, b, sizeof(b));
}

static u32 vht_cap(u8 *p)
{
    u8 b[12];

    put32(b, VHT_CAP_INFO);
    put16(b + 4, VHT_MCS_MAP);
    put16(b + 6, 780);
    put16(b + 8, VHT_MCS_MAP);
    put16(b + 10, 780);
    return ie(p, IE_VHT_CAP, b, sizeof(b));
}

u32 wl_frame_probe_req(u8 *f, const u8 *own, u32 seq, u32 channel)
{
    static const u8 broadcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    u32 n = header(f, 4, broadcast, own, broadcast, seq);

    n += ie(f + n, IE_SSID, 0, 0);          /* any network */
    n += rates(f + n, channel);
    n += ht_cap(f + n);
    if (channel > 14)
        n += vht_cap(f + n);
    return n;
}

u32 wl_frame_auth(u8 *f, const u8 *own, const u8 *bssid, u32 seq)
{
    u32 n = header(f, 11, bssid, own, bssid, seq);

    put16(f + n, 0);                        /* open system */
    put16(f + n + 2, 1);                    /* transaction 1 */
    put16(f + n + 4, 0);                    /* status */
    return n + 6;
}

u32 wl_frame_deauth(u8 *f, const u8 *own, const u8 *bssid, u32 seq, u32 reason)
{
    u32 n = header(f, 12, bssid, own, bssid, seq);

    put16(f + n, reason);
    return n + 2;
}

/* The association request: the network's name, our rates, its security element if
 * secured (the one the handshake will expect), HT and VHT if the AP does them,
 * and WMM if it announces it (QoS data needs it, and so does HT). */
u32 wl_frame_assoc_req(u8 *f, const u8 *own, const WL_BSS *bss, const WL_AP_IES *ap,
                       const u8 *rsn, u32 rsn_len, u32 seq)
{
    static const u8 wmm[] = { 0x00, 0x50, 0xF2, 0x02, 0x00, 0x01, 0x00 };
    u32 n = header(f, 0, bss->bssid, own, bss->bssid, seq), cap;

    cap = 0x0001 | (bss->capability & 0x0010) | 0x0020 | 0x0400; /* ESS, privacy,
                                                                     * short preamble,
                                                                     * short slot */
    put16(f + n, cap);
    put16(f + n + 2, 10);                   /* listen interval */
    n += 4;
    n += ie(f + n, IE_SSID, (const u8 *)bss->ssid, bss->ssid_len);
    n += rates(f + n, bss->channel);
    if (rsn_len)
        n += put(f + n, rsn, rsn_len);
    if (ap->ht)
        n += ht_cap(f + n);
    if (ap->vht && bss->channel > 14)
        n += vht_cap(f + n);
    if (ap->wmm)
        n += ie(f + n, IE_VENDOR, wmm, sizeof(wmm));
    return n;
}

/* --- block ack ---------------------------------------------------------------------- */

/* An ADDBA request (action category 3, action 0): immediate block ack for `tid`, 64
 * frames, no timeout, starting at `ssn`. */
u32 wl_frame_addba_req(u8 *f, const u8 *own, const u8 *bssid, u32 token, u32 tid, u32 ssn)
{
    u32 n = header(f, 13, bssid, own, bssid, 0);

    f[n++] = 3;
    f[n++] = 0;
    f[n++] = (u8)token;
    put16(f + n, 1u << 1 | (tid & 15) << 2 | 64u << 6);    /* immediate, TID, 64 */
    put16(f + n + 2, 0);                                    /* timeout */
    put16(f + n + 4, (ssn & 0xFFF) << 4);
    return n + 6;
}

/* An ADDBA response: `param` the block-ack parameter set as answered. */
u32 wl_frame_addba_resp(u8 *f, const u8 *own, const u8 *bssid, u32 token, u32 status,
                        u32 param, u32 timeout)
{
    u32 n = header(f, 13, bssid, own, bssid, 0);

    f[n++] = 3;
    f[n++] = 1;
    f[n++] = (u8)token;
    put16(f + n, status);
    put16(f + n + 2, param);
    put16(f + n + 4, timeout);
    return n + 6;
}

/* --- reading the AP ---------------------------------------------------------------- */

static void legacy_rates(const u8 *e, u32 n, u32 *mask)
{
    static const u8 table[] = { 2, 4, 11, 22, 12, 18, 24, 36, 48, 72, 96, 108 };
    u32 i, k;

    for (i = 0; i < n; i++)
        for (k = 0; k < sizeof(table); k++)
            if ((e[i] & 0x7F) == table[k])
                *mask |= 1u << k;
}

/* What a beacon or probe response's elements say the link may use, starting from
 * byte `at` of `frame`. */
void wl_parse_ap(const u8 *ies, u32 length, WL_AP_IES *ap)
{
    u32 at;

    ap->ht = ap->vht = ap->wmm = 0;
    ap->legacy_rates = 0;
    ap->ht_mcs = 0;
    ap->vht_mcs_map = 0xFFFF;
    ap->second_above = ap->second_below = 0;
    ap->vht_width = 0;
    ap->vht_center = 0;
    ap->ht_sgi20 = ap->ht_sgi40 = ap->vht_sgi80 = 0;
    ap->ampdu_factor = 0;
    ap->ampdu_density = 0;
    ap->rx_ldpc = ap->rx_stbc = 0;
    for (at = 0; at + 2 <= length; at += 2 + ies[at + 1]) {
        const u8 *e = ies + at + 2;
        u32 n = ies[at + 1];

        if (at + 2 + n > length)
            break;
        switch (ies[at]) {
        case IE_RATES:
        case IE_EXT_RATES:
            legacy_rates(e, n, &ap->legacy_rates);
            break;
        case IE_HT_CAP:
            if (n >= 26) {
                ap->ht = 1;
                ap->ht_sgi20 = (e[0] >> 5) & 1;
                ap->ht_sgi40 = (e[0] >> 6) & 1;
                ap->ampdu_factor = e[2] & 3;
                ap->ampdu_density = (e[2] >> 2) & 7;
                ap->ht_mcs = (u16)(e[3] | e[4] << 8);
                ap->rx_ldpc |= e[0] & 1;
                ap->rx_stbc |= (e[1] & 3) != 0;
            }
            break;
        case IE_HT_OP:
            if (n >= 2) {
                ap->second_above = (e[1] & 3) == 1;
                ap->second_below = (e[1] & 3) == 3;
                if (!(e[1] & 4))            /* STA channel width: 20 only */
                    ap->second_above = ap->second_below = 0;
            }
            break;
        case IE_VHT_CAP:
            if (n >= 12) {
                ap->vht = 1;
                ap->vht_sgi80 = (e[0] >> 5) & 1;
                ap->vht_mcs_map = (u16)le16(e + 4);
                ap->rx_ldpc |= (e[0] >> 4) & 1;
                ap->rx_stbc |= (e[1] & 7) != 0;
            }
            break;
        case IE_VHT_OP:
            if (n >= 3) {
                ap->vht_width = e[0];
                ap->vht_center = e[1];
            }
            break;
        case IE_VENDOR:
            if (n >= 4 && e[0] == 0x00 && e[1] == 0x50 && e[2] == 0xF2 && e[3] == 2)
                ap->wmm = 1;
            break;
        default:
            break;
        }
    }
}

/* The link the station will use: 80 MHz when the AP runs VHT at 80, else 40 when
 * it has a secondary channel, else 20; rtw88's RTW_SC_* for where the primary sits
 * in it (1 upper/2 lower of 40; at 80 also 3 upmost and 4 lowest). */
void wl_link_from_ap(const WL_BSS *bss, const WL_AP_IES *ap, WL_LINK_INFO *link)
{
    u32 i, ch = bss->channel;

    for (i = 0; i < 6; i++)
        link->bssid[i] = bss->bssid[i];
    link->channel = (u8)ch;
    link->ht = ap->ht;
    link->vht = ap->vht && ch > 14;
    link->legacy_rates = ap->legacy_rates;
    link->ht_mcs = ap->ht_mcs;
    link->vht_mcs_map = ap->vht_mcs_map;
    link->ampdu_factor = ap->ampdu_factor;
    link->ampdu_density = ap->ampdu_density;
    link->ldpc = ap->rx_ldpc;
    link->stbc = ap->rx_stbc;
    link->bw = 0;
    link->primary = 0;
    link->sgi = ap->ht_sgi20;
    if (link->vht && ap->vht_width == 1 && ap->vht_center) {
        u32 c = ap->vht_center;             /* center of the 80 MHz channel */

        link->bw = 2;
        link->primary = ch == c - 6 ? 4 : ch == c - 2 ? 2 : ch == c + 2 ? 1 : 3;
        link->sgi = ap->vht_sgi80;
    } else if (ap->ht && (ap->second_above || ap->second_below)) {
        link->bw = 1;
        link->primary = ap->second_above ? 2 : 1;   /* above: primary is the lower */
        link->sgi = ap->ht_sgi40;
    }
}
