/* The SoftMAC station's management: scanning, joining, staying joined, leaving.
 *
 * The receive path (DISPATCH) only notes what came -- a beacon's elements, the
 * answer to an authentication or association, a deauthentication -- and the
 * worker's tick acts on it, because tuning and setting up the link wait for the
 * card. A join: on the network's channel until one of its beacons gives its
 * elements, open-system authentication, association with what both sides do
 * (frames.c), then for WPA2 the handshake (wpa.c), whose keys the card gets and
 * whose end opens the port to the network layer.
 */

#include "station.h"
#include "frames.h"
#include "wpa.h"

#define DWELL_TICKS     3               /* ~100 ms of 33 ms ticks on a channel */
#define STEP_TICKS      15              /* ~0.5 s for an answer */
#define STEP_TRIES      3
#define LOSS_TICKS      150             /* ~5 s without a beacon from the AP */
#define REASON_LEAVING  3

static const u8 scan_channels[] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
    36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128, 132,
    136, 140, 144, 149, 153, 157, 161, 165,
};

static u32 freq_of(u32 channel)
{
    return channel <= 14 ? 2407 + 5 * channel : 5000 + 5 * channel;
}

/* Radar channels are listened on, not probed. */
static u32 passive(u32 channel)
{
    return channel >= 52 && channel <= 144;
}

static u32 same(const u8 *a, const u8 *b)
{
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3] && a[4] == b[4] &&
           a[5] == b[5];
}

/* A structure assigned whole would be a memcpy, which the plugin has none of. */
static void copy(void *to, const void *from, u32 n)
{
    const u8 *f = (const u8 *)from;
    u8 *t = (u8 *)to;
    u32 i;

    for (i = 0; i < n; i++)
        t[i] = f[i];
}

static void send_mgmt(WL_STA *s, const u8 *f, u32 length)
{
    s->hw->tx(s->hw->context, f, length, 0, 0, WL_TX_MGMT, 0);
}

/* --- the handshake's host (wpa.h) ------------------------------------------------- */

static u32 wpa_key(void *context, u32 pairwise, u32 index, const u8 *key, u32 length)
{
    WL_STA *s = (WL_STA *)context;
    u32 ok = s->hw->set_key(s->hw->context, pairwise, index, key, length, s->bss.bssid);

    if (ok && pairwise) {
        s->pn_lo = 0;
        s->pn_hi = 0;
        mem_sync();
        s->pairwise_set = 1;
    }
    s->st->keys++;
    return ok;
}

static void wpa_ready(void *context, u32 ok)
{
    WL_STA *s = (WL_STA *)context;

    s->port_open = ok;
    s->st->port_open = ok;
    mem_sync();
    net_link_report(s->nd, ok);
}

/* --- receive ---------------------------------------------------------------------- */

void wl_sta_rx(WL_STA *s, u8 *f, u32 length, s32 signal, u32 channel, u32 decrypted)
{
    u32 type, subtype, i;

    if (length < 24)
        return;
    type = (f[0] >> 2) & 3;
    subtype = f[0] >> 4;
    if (type == 2) {
        /* the handshake's first message follows the association answer at once,
         * before the worker has acted on it */
        if (s->state == WL_STA_JOINED || (s->state == WL_STA_ASSOC && s->got_assoc))
            wl_sta_rx_data(s, f, length, decrypted);
        return;
    }
    if (type != 0)
        return;

    if (subtype == 8 || subtype == 5) {                 /* beacon, probe response */
        if (s->state == WL_STA_SCANNING) {
            wl_scan_result(f, length, freq_of(channel), signal);
            return;
        }
        if (!same(f + 16, s->bss.bssid))
            return;
        s->last_beacon = 0;
        if (s->state == WL_STA_PROBING && !s->ies_len && length > 36) {
            u32 n = length - 36 < WL_STA_IES_MAX ? length - 36 : WL_STA_IES_MAX;

            for (i = 0; i < n; i++)
                s->ies[i] = f[36 + i];
            mem_sync();
            s->ies_len = n;
        }
        return;
    }
    if (!same(f + 4, s->own) || !same(f + 16, s->bss.bssid))
        return;
    switch (subtype) {
    case 11:                                            /* authentication */
        if (length >= 30 && (f[24] | f[25] << 8) == 0 && (f[26] | f[27] << 8) == 2) {
            s->st->auth_status = f[28] | f[29] << 8;
            mem_sync();
            s->got_auth = 1;
        }
        break;
    case 1:                                             /* association response */
    case 3:                                             /* reassociation response */
        if (length >= 30) {
            s->st->assoc_status = f[26] | f[27] << 8;
            s->st->aid = (f[28] | f[29] << 8) & 0x3FFF;
            mem_sync();
            s->got_assoc = 1;
        }
        break;
    case 13:                                            /* action */
        if (s->state == WL_STA_JOINED)
            wl_sta_action(s, f, length);
        break;
    case 10:                                            /* disassociation */
    case 12:                                            /* deauthentication */
        s->st->deauths++;
        s->st->deauth_reason = length >= 26 ? f[24] | f[25] << 8 : 0;
        mem_sync();
        s->got_lost = 1;
        break;
    default:
        break;
    }
}

/* --- the card, to the layer above (WL_CARD) -------------------------------------- */

static u32 card_scan(void *context)
{
    WL_STA *s = (WL_STA *)context;

    if (s->state != WL_STA_IDLE)
        return STATUS_UNSUCCESSFUL;
    s->scan_index = 0;
    s->ticks = 0;
    s->st->scans++;
    wl_scan_begin();
    mem_sync();
    s->state = WL_STA_SCANNING;
    return 0;
}

static void link_down(WL_STA *s)
{
    wl_sta_ba_reset(s);
    s->port_open = 0;
    s->pairwise_set = 0;
    s->st->port_open = 0;
    mem_sync();
    s->hw->set_link(s->hw->context, 0);
    s->hw->clear_keys(s->hw->context);
    net_link_report(s->nd, 0);
}

static u32 card_leave(void *context)
{
    WL_STA *s = (WL_STA *)context;
    u8 f[WL_FRAME_MAX];

    if (s->state == WL_STA_JOINED) {
        send_mgmt(s, f, wl_frame_deauth(f, s->own, s->bss.bssid, 0, REASON_LEAVING));
        link_down(s);
    }
    s->state = WL_STA_IDLE;
    s->st->state = WL_STA_IDLE;
    return 0;
}

/* The address to join on: the console's, as its network stack sends from it (the
 * same reason as aic_wifi.c's join_mac), else the card's own. */
static void choose_own(WL_STA *s)
{
    const u8 *mac = net_console_mac_known ? net_console_mac : s->nd->mac;
    u32 i;

    for (i = 0; i < 6; i++)
        s->own[i] = mac[i];
    s->hw->set_mac(s->hw->context, s->own);
}

static u32 card_join(void *context, const WL_BSS *bss, const char *pass, u32 pass_len)
{
    WL_STA *s = (WL_STA *)context;
    u32 i;

    if (s->state == WL_STA_JOINED)
        card_leave(s);
    copy(&s->bss, bss, sizeof(s->bss));
    for (i = 0; i < pass_len && i < sizeof(s->pass) - 1; i++)
        s->pass[i] = pass[i];
    s->pass[i] = 0;
    s->pass_len = i;
    s->secured = (bss->security & WL_SEC_WPA2) != 0;
    if (s->secured)
        wpa_set_psk(s->pass, (const u8 *)bss->ssid, bss->ssid_len);
    choose_own(s);
    s->ies_len = 0;
    s->got_auth = s->got_assoc = s->got_lost = 0;
    s->tries = 0;
    s->ticks = 0;
    s->last_beacon = 0;
    s->hw->set_channel(s->hw->context, bss->channel, 0, 0);
    mem_sync();
    s->state = WL_STA_PROBING;
    s->st->state = WL_STA_PROBING;
    return 0;
}

static u32 card_state(void *context)
{
    WL_STA *s = (WL_STA *)context;

    switch (s->state) {
    case WL_STA_SCANNING:
        return WL_CARD_SCANNING;
    case WL_STA_IDLE:
        return 0;
    default:                                /* joining counts as joined: no rescans */
        return WL_CARD_JOINED;
    }
}

/* --- the worker ----------------------------------------------------------------- */

static void step_to(WL_STA *s, u32 state)
{
    s->state = state;
    s->st->state = state;
    s->ticks = 0;
}

static void give_up(WL_STA *s)
{
    s->st->timeouts++;
    step_to(s, WL_STA_IDLE);
}

static void send_auth(WL_STA *s)
{
    u8 f[WL_FRAME_MAX];

    s->got_auth = 0;
    s->st->auths++;
    send_mgmt(s, f, wl_frame_auth(f, s->own, s->bss.bssid, 0));
    step_to(s, WL_STA_AUTH);
}

static void send_assoc(WL_STA *s)
{
    WL_AP_IES ap;
    u8 f[WL_FRAME_MAX];
    WPA_HOST host;

    wl_parse_ap(s->ies, s->ies_len, &ap);
    s->rsn_len = s->secured ? wpa_rsn_ie(s->rsn) : 0;
    if (s->secured) {
        /* before the request: the AP's first message follows its answer at once */
        host.send = wl_sta_send_eapol;
        host.key = wpa_key;
        host.ready = wpa_ready;
        host.context = s;
        wpa_begin(&host, s->own, s->bss.bssid, s->rsn, s->rsn_len);
    }
    s->got_assoc = 0;
    s->st->assocs++;
    send_mgmt(s, f, wl_frame_assoc_req(f, s->own, &s->bss, &ap, s->rsn, s->rsn_len, 0));
    step_to(s, WL_STA_ASSOC);
}

static void joined(WL_STA *s)
{
    WL_AP_IES ap;
    u32 i;

    wl_parse_ap(s->ies, s->ies_len, &ap);
    wl_link_from_ap(&s->bss, &ap, &s->link);
    s->link.aid = (u16)s->st->aid;
    s->st->bw = s->link.bw;
    s->st->channel = s->link.channel;
    s->seq = 0;
    s->last_beacon = 0;
    for (i = 0; i <= WL_REORDER_TIDS; i++)
        s->last_seq_ctrl[i] = 0xFFFF;
    wl_sta_ba_reset(s);
    s->hw->set_link(s->hw->context, &s->link);
    s->st->joins++;
    step_to(s, WL_STA_JOINED);
    if (!s->secured)
        wpa_ready(s, 1);
}

void wl_sta_tick(WL_STA *s)
{
    u8 f[WL_FRAME_MAX];
    u32 ch;

    s->ticks++;
    switch (s->state) {
    case WL_STA_SCANNING:
        if (s->ticks == 1) {
            ch = scan_channels[s->scan_index];
            s->hw->set_channel(s->hw->context, ch, 0, 0);
            s->bss.channel = (u8)ch;
            if (!passive(ch)) {
                s->st->probes++;
                send_mgmt(s, f, wl_frame_probe_req(f, s->own, 0, ch));
            }
        }
        if (s->ticks < DWELL_TICKS)
            break;
        s->ticks = 0;
        if (++s->scan_index < sizeof(scan_channels))
            break;
        wl_scan_end(0);
        step_to(s, WL_STA_IDLE);
        break;

    case WL_STA_PROBING:
        if (s->ies_len) {
            send_auth(s);
        } else if (s->ticks >= STEP_TICKS) {
            if (++s->tries > STEP_TRIES) {
                give_up(s);
                break;
            }
            s->ticks = 0;
            s->st->probes++;
            send_mgmt(s, f, wl_frame_probe_req(f, s->own, 0, s->bss.channel));
        }
        break;

    case WL_STA_AUTH:
        if (s->got_auth) {
            if (s->st->auth_status)
                give_up(s);
            else
                send_assoc(s);
        } else if (s->ticks >= STEP_TICKS) {
            if (++s->tries > STEP_TRIES)
                give_up(s);
            else
                send_auth(s);
        }
        break;

    case WL_STA_ASSOC:
        if (s->got_assoc) {
            if (s->st->assoc_status)
                give_up(s);
            else
                joined(s);
        } else if (s->ticks >= STEP_TICKS) {
            if (++s->tries > STEP_TRIES)
                give_up(s);
            else
                send_assoc(s);
        }
        break;

    case WL_STA_JOINED:
        wl_sta_ba_tick(s);
        if (s->got_lost) {
            s->got_lost = 0;
            link_down(s);
            step_to(s, WL_STA_IDLE);
        } else if (++s->last_beacon > LOSS_TICKS) {
            s->st->beacon_losses++;
            link_down(s);
            step_to(s, WL_STA_IDLE);
        }
        break;

    default:
        break;
    }
}

static void reorder_deliver(void *context, u8 *eth, u32 length)
{
    WL_STA *s = (WL_STA *)context;

    net_frame_in(s->nd, eth, length);
}

void wl_sta_init(WL_STA *s, const WL_SOFTMAC *hw, NET_DEV *nd, volatile WL_STA_STATS *st)
{
    s->hw = hw;
    s->nd = nd;
    s->st = st;
    s->state = WL_STA_IDLE;
    s->port_open = 0;
    s->pairwise_set = 0;
    s->card.context = s;
    s->card.scan = card_scan;
    s->card.join = card_join;
    s->card.leave = card_leave;
    s->card.state = card_state;
    s->rx_ba = 0;
    s->tx_ba = 0;
    wl_reorder_init(&s->reorder, s->reorder_pool, reorder_deliver, s, &st->reorder);
    s->reorder.drop_late = 1;           /* a duplicate, as mac80211 has it */
    wl_sta_ba_init(s);
    choose_own(s);
}
