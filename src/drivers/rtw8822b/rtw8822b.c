/* Realtek RTL8822BU: the driver the core sees (rtw.h for the chip).
 *
 * Brought up as rtw88's probe, power_on and core_start do it: the MAC powered,
 * the firmware downloaded and running, the efuse read for the card's address,
 * the MAC, baseband and radio set up, a station's port on that address, and
 * receive running. Scanning, joining and the data path are the SoftMAC station's
 * (src/wireless/softmac/), which this file gives the card's operations to; which
 * network to join and when is the Wi-Fi mode's (src/wireless/mode, wl_mode_pick),
 * as for any Wi-Fi card.
 */

#include "rtw.h"
#include "bench.h"
#include "rtw8822b_defs.h"
#include "driver.h"
#include "wireless.h"
#include "bridge.h"

#define RTW_MAX             2
#define REG_BUF_SIZE        64
#define BULK_BUF_SIZE       8192        /* a firmware block, 4 KB and its descriptor */
#define C2H_HW_FEATURE_DUMP 0xFD
#define RCR_SCAN            0xF410400Eu /* rtw88's hal.rcr, no BSSID filters */
#define SEC_ENGINE_EN       BIT(9)
#define SEC_CONFIG          0x680
#define SEC_DEFAULT         0xCF        /* decrypt both ways, default keys */
/* rtw_vif_port[0], mac80211.c */
#define PORT0_MAC           0x0610
#define PORT0_BSSID         0x0618
#define PORT0_NET_TYPE      0x30000     /* in REG_CR */
#define PORT0_AID           0x06A8
#define PORT0_BCN_CTRL      0x0550
#define NET_MGD_LINKED      2           /* RTW_NET_MGD_LINKED */
#define RCR_LINKED          (RCR_SCAN | BIT_CBSSID_BCN | BIT_CBSSID_DATA)
/* EDCCA, energy-detect CCA: the thresholds (low bytes, L2H 0x48 / H2L 0x40) above
 * which the card holds off transmitting. The tables leave both at 0x7F, never;
 * rtw88 sets these once associated (rtw_phy_adaptivity, seen in usbmon) and puts
 * 0x7F back without a link. On 2.4 GHz channel 10, 14-16% busy, A/B twice
 * (2026-11-03): 4 streams out 40.5 / 41.0 -> 45.1 / 43.8 Mbit/s, in alike. */
#define REG_EDCCA_TH        0x08A4
#define EDCCA_LINKED        0x7F7F4048u
#define EDCCA_OFF           0x7F7F7F7Fu
#define DESC_RATE54M        0x0B
#define DESC_RATEMCS15      0x1B
#define DESC_RATEVHT2SS_MCS0 0x36
#define RATEID_BGN_40M_2SS  0
#define RATEID_BGN_20M_2SS  2
#define RATEID_GN_N2SS      4
#define RATEID_BG           6
#define RATEID_G            7
#define RATEID_AC_2SS       9           /* RTW_RATEID_ARFR0_AC_2SS */
#define RATEID_AC_2G_2SS    12          /* RTW_RATEID_ARFR3_AC_2G_2SS */
#define MACID_AP            1

volatile RTW_STATS rtw_stats[USB_MAX_DEVICES];
volatile WL_STA_STATS rtw_sta_stats[USB_MAX_DEVICES];

static RTW rtw_inst[RTW_MAX];
static u8 efuse_maps[RTW_MAX][RTW_EFUSE_LOGICAL];

/* The products rtw88_8822bu takes that are known here; more as they are tried. */
static u32 rtw8822b_match(const u8 *device, const u8 *interface)
{
    u32 vendor = (u32)(device[8] | device[9] << 8);
    u32 product = (u32)(device[10] | device[11] << 8);

    if (interface && interface[5] != 0xFF)
        return XE_MATCH_NONE;
    if (vendor == RTW_VENDOR && product == 0xB812)
        return XE_MATCH_VENDOR;
    return XE_MATCH_NONE;
}

static u32 op_tx(void *c, const u8 *head, u32 head_length, const u8 *body, u32 body_length,
                 u32 flags, u32 tid);
static u32 op_tx_space(void *c);
static void op_set_channel(void *c, u32 channel, u32 bw, u32 primary);
static void op_set_mac(void *c, const u8 *mac);
static void op_set_link(void *c, const WL_LINK_INFO *link);
static u32 op_set_key(void *c, u32 pairwise, u32 index, const u8 *key, u32 length,
                      const u8 *peer);
static void op_clear_keys(void *c);
static void op_set_aggregation(void *c, u32 tid, u32 on);

static RTW *take_instance(NET_DEV *nd)
{
    u32 i;

    for (i = 0; i < RTW_MAX; i++) {
        RTW *r = &rtw_inst[i];

        if (r->in_use && r->nd != nd)
            continue;
        r->in_use = 1;
        r->nd = nd;
        r->dev = nd->usb;
        r->st = &rtw_stats[nd->usb->index];
        r->efuse = efuse_maps[i];
        r->ops.context = r;
        r->ops.tx = op_tx;
        r->ops.tx_space = op_tx_space;
        r->ops.set_channel = op_set_channel;
        r->ops.set_mac = op_set_mac;
        r->ops.set_link = op_set_link;
        r->ops.set_key = op_set_key;
        r->ops.clear_keys = op_clear_keys;
        r->ops.set_aggregation = op_set_aggregation;
        nd->priv = r;
        return r;
    }
    return 0;
}

static u32 fail(RTW *r, u32 step, u32 status)
{
    r->st->step = step;
    r->st->step_status = status;
    return step;
}

static u32 rtw8822b_start(NET_DEV *nd)
{
    RTW *r = take_instance(nd);
    u32 status, step;

    if (!r)
        return RTW_STEP_DMA;
    r->st->step = 0;
    if (!net_dma_begin(nd, REG_BUF_SIZE + BULK_BUF_SIZE + RTW_RX_COUNT * RTW_RX_SIZE +
                       RTW_TX_COUNT * RTW_TX_SLOT + (RTW_RX_COUNT + RTW_TX_COUNT + 2) * 2048))
        return fail(r, RTW_STEP_DMA, 0);
    r->reg_buf = (u8 *)net_dma_alloc(nd, REG_BUF_SIZE);
    r->bulk_buf = (u8 *)net_dma_alloc(nd, BULK_BUF_SIZE);
    for (step = 0; step < RTW_RX_COUNT; step++)
        r->rx_buf[step] = (u8 *)net_dma_alloc(nd, RTW_RX_SIZE);
    for (step = 0; step < RTW_TX_COUNT; step++) {
        r->tx_buf[step] = (u8 *)net_dma_alloc(nd, RTW_TX_SLOT);
        r->tx_busy[step] = 0;
    }
    r->tx_inflight = 0;
    r->tx_lock = 0;
    /* a fresh firmware takes commands from mailbox 0 and packets from sequence
     * 0: left where the last bring-up stopped, after a reset the media status
     * and RA info went to a mailbox it never read, and the link ran at 6M */
    r->h2c_box = 0;
    r->h2c_seq = 0;
    for (step = 0; step < RTW_OUT_MAX; step++) {
        r->tx_ep_busy[step] = 0;
        r->tx_open[step] = RTW_TX_COUNT;
    }
    if (!r->reg_buf || !r->bulk_buf || !r->rx_buf[RTW_RX_COUNT - 1] ||
        !r->tx_buf[RTW_TX_COUNT - 1])
        return fail(r, RTW_STEP_DMA, 0);

    step = rtw_usb_open(r);
    if (step)
        return fail(r, step, 0);

    /* rtw_chip_parameter_setup: the version, and its cut */
    r->st->chip_version = rtw_read32(r, REG_SYS_CFG1);
    r->cut = BIT_GET_CHIP_VER(r->st->chip_version);
    r->st->cut = r->cut;
    if (r->st->reg_errors)
        return fail(r, RTW_STEP_CHIP, r->st->reg_last_error);

    /* rtw_chip_efuse_enable */
    status = rtw_mac_power_on(r);
    if (status)
        return fail(r, RTW_STEP_POWER_ON, status);
    rtw_write8(r, REG_C2HEVT, C2H_HW_FEATURE_DUMP);
    step = rtw_download_firmware(r);
    if (step)
        return fail(r, step, 0);

    status = rtw_parse_efuse_map(r);
    if (status)
        return fail(r, RTW_STEP_EFUSE, status);

    /* the rest of rtw_power_on */
    step = rtw_mac_init(r);
    if (step)
        return fail(r, step, 0);
    step = rtw_phy_set_param(r);
    if (step)
        return fail(r, step, 0);
    rtw_fw_send_general_info(r);
    rtw_fw_send_phydm_info(r);

    /* rtw_core_start: the security engine, the receive configuration */
    rtw_write16(r, REG_CR, (u16)(rtw_read16(r, REG_CR) | SEC_ENGINE_EN));
    rtw_write16(r, SEC_CONFIG, (u16)(rtw_read16(r, SEC_CONFIG) | SEC_DEFAULT));
    rtw_write32(r, REG_RCR, RCR_SCAN);

    /* rtw_ops_add_interface, a station on port 0: the address, no link yet,
     * beacon functions on */
    for (step = 0; step < 6; step++)
        rtw_write8(r, PORT0_MAC + step, nd->mac[step]);
    rtw_write32_mask(r, REG_CR, PORT0_NET_TYPE, 0);          /* RTW_NET_NO_LINK */
    rtw_write8_mask(r, PORT0_BCN_CTRL, 0xFF, BIT_EN_BCN_FUNCTION);

    rtw_set_channel(r, 36, 0, 0);
    r->tx_ready = 1;
    rtw_rx_start(r);
    r->st->step = RTW_STEP_UP;

    /* the station, and the mode that decides what it joins */
    wl_sta_init(&r->sta, &r->ops, nd, &rtw_sta_stats[nd->usb->index]);
    wl_load_config();
    r->link.card = &r->sta.card;
    r->link.mode = wl_mode_pick();
    r->link.mode->start(&r->link, nd);
    return 0;
}

/* --- the card's operations for the station (softmac.h) ---------------------------- */

static u32 op_tx(void *c, const u8 *head, u32 head_length, const u8 *body, u32 body_length,
                 u32 flags, u32 tid)
{
    return rtw_tx((RTW *)c, head, head_length, body, body_length, flags, tid);
}

static u32 op_tx_space(void *c)
{
    return rtw_tx_space((RTW *)c);
}

static void op_set_channel(void *c, u32 channel, u32 bw, u32 primary)
{
    rtw_set_channel((RTW *)c, channel, bw, primary);
}

static void op_set_mac(void *c, const u8 *mac)
{
    RTW *r = (RTW *)c;
    u32 i;

    for (i = 0; i < 6; i++)
        rtw_write8(r, PORT0_MAC + i, mac[i]);
}

/* The center of a 40 or 80 MHz channel from its primary and rtw88's RTW_SC_* of
 * where the primary sits (1 upper, 2 lower, 3 upmost, 4 lowest). */
static u32 center_of(u32 channel, u32 bw, u32 primary)
{
    if (bw == 1)
        return primary == 2 ? channel + 2 : channel - 2;
    if (bw == 2)
        return primary == 4 ? channel + 6 : primary == 2 ? channel + 2 :
               primary == 1 ? channel - 2 : channel - 6;
    return channel;
}

/* rtw_update_sta_info for the AP: the rates both sides do, the firmware's rate set
 * for them (rate_id), the best of them to start from. Two streams each way. */
static void rate_info(RTW *r, const WL_LINK_INFO *l, u32 *mask, u32 *rate_id, u32 *top)
{
    u32 m = 0, i, cap, legacy = l->legacy_rates;

    if (l->vht) {
        u32 map = l->vht_mcs_map;

        for (i = 0; i < 2; i++, map >>= 2) {             /* get_vht_ra_mask, 2SS */
            cap = map & 3;
            if (cap == 2)
                m |= 0x3FFu << (12 + 10 * i);
            else if (cap == 1)
                m |= 0x1FFu << (12 + 10 * i);
            else if (cap == 0)
                m |= 0x0FFu << (12 + 10 * i);
        }
        cap = (l->vht_mcs_map >> 2) & 3;
        *top = DESC_RATEVHT2SS_MCS0 + (cap == 0 ? 7 : cap == 1 ? 8 : 9);
    } else if (l->ht) {
        m = (u32)l->ht_mcs << 12;
        *top = DESC_RATEMCS15;
    } else {
        *top = DESC_RATE54M;
    }
    if (r->channel > 14) {
        m |= legacy & 0xFF0;
        if (l->vht) {
            m &= 0xFFFFF000u | 0x10;                    /* VHT, OFDM_IN_VHT */
            *rate_id = RATEID_AC_2SS;
        } else if (l->ht) {
            m &= 0x0FFFF000u | 0x30;                    /* HT, OFDM_IN_HT_5G */
            *rate_id = RATEID_GN_N2SS;
        } else {
            *rate_id = RATEID_G;
        }
    } else {
        m |= legacy;
        if (l->vht) {
            m &= 0xFFFFF000u | 0x5 | 0x10;
            *rate_id = l->bw >= 2 ? RATEID_AC_2SS : RATEID_AC_2G_2SS;
        } else if (l->ht) {
            m &= 0x0FFFF000u | 0x5 | 0x10;
            *rate_id = l->bw == 1 ? RATEID_BGN_40M_2SS : RATEID_BGN_20M_2SS;
        } else {
            m &= 0xFF0 | 0x5;
            *rate_id = RATEID_BG;
        }
    }
    if (!m)                                             /* rtw_rate_mask_recover */
        m = legacy;
    *mask = m;
}

/* rtw_ops_bss_info_changed on association: the BSSID, a managed link, the AID, the
 * BSSID filters on, the channel at the link's width, the AP's media status and its
 * rates to the firmware, which picks the data rate from then on; leaving undoes
 * it. */
static void op_set_link(void *c, const WL_LINK_INFO *link)
{
    RTW *r = (RTW *)c;
    u32 i;

    if (!link) {
        r->ampdu = 0;
        /* No "disconnected" to the firmware for the AP's MAC ID: rtw88 reports it
         * when mac80211 removes the station and "connected" again when it adds the
         * next, before the four-way. Here the next "connected" comes only with the
         * link, after it, and the firmware held every frame for a disconnected MAC
         * ID -- the handshake's answers among them: after a deauthentication every
         * join failed at the four-way (2026-11-04, hostapd resent message 1 four
         * times, nothing came back). The next link rewrites it and its rates. */
        rtw_write32(r, REG_RCR, RCR_SCAN);
        rtw_write32(r, REG_EDCCA_TH, EDCCA_OFF);
        rtw_write32_mask(r, REG_CR, PORT0_NET_TYPE, 0);
        for (i = 0; i < 6; i++)
            rtw_write8(r, PORT0_BSSID + i, 0);
        r->st->linked = 0;
        rtw_dig_reset(r);
        return;
    }
    rtw_set_channel(r, center_of(link->channel, link->bw, link->primary), link->bw,
                    link->primary);
    rtw_iqk(r);
    for (i = 0; i < 6; i++) {
        rtw_write8(r, PORT0_BSSID + i, link->bssid[i]);
        r->bssid[i] = link->bssid[i];
    }
    rtw_write32_mask(r, REG_CR, PORT0_NET_TYPE, NET_MGD_LINKED);
    rtw_write32_mask(r, PORT0_AID, 0x7FF, link->aid);
    rtw_write32(r, REG_RCR, RCR_LINKED);
    /* wifi_edcca: rtw88's threshold for the gain at its link (static), the DIG's
     * from here on (dynamic, rtw_phy.c), or never holding off (off) */
    rtw_write32(r, REG_EDCCA_TH, wl_config.edcca == WL_EDCCA_OFF ? EDCCA_OFF : EDCCA_LINKED);
    {
        u32 mask, rate_id, top;

        rate_info(r, link, &mask, &rate_id, &top);
        rtw_fw_media_status(r, MACID_AP, 1);
        rtw_fw_ra_info(r, MACID_AP, rate_id, 1, link->sgi, link->bw, link->ldpc,
                       link->vht, mask);
        r->data_rate = top;
        r->data_rate_id = rate_id;
        r->data_use_rate = 0;
        r->st->ra_mask = mask;
        r->st->ra_rate_id = rate_id;
    }
    r->data_bw = link->bw;
    r->data_sgi = 0;                    /* the firmware's, with its rate */
    r->data_ldpc = link->ldpc;
    r->data_stbc = link->stbc;
    r->ampdu_factor = (4u << link->ampdu_factor) - 1;   /* get_tx_ampdu_factor */
    r->ampdu_density = link->ampdu_density;
    rtw_dig_reset(r);
    r->st->linked = 1;
}

static void op_set_aggregation(void *c, u32 tid, u32 on)
{
    RTW *r = (RTW *)c;

    (void)tid;
    r->ampdu = on;
    r->st->ampdu = on;
}

static u32 op_set_key(void *c, u32 pairwise, u32 index, const u8 *key, u32 length,
                      const u8 *peer)
{
    return rtw_set_key((RTW *)c, pairwise, index, key, length, peer);
}

static void op_clear_keys(void *c)
{
    rtw_clear_keys((RTW *)c);
}

/* A received frame goes to the station: decrypted when the card says it did
 * (rtw_rx_query_rx_desc: an encryption type and no software decryption). */
void rtw_rx_frame(RTW *r, u8 *frame, u32 length, s32 signal, u32 desc_w0)
{
    u32 type = (frame[0] >> 2) & 3;

    r->st->last_signal = signal;
    if (r->st->linked && signal > -100 && length >= 16 &&       /* the AP's, for the DIG */
        frame[10] == r->bssid[0] && frame[11] == r->bssid[1] && frame[12] == r->bssid[2] &&
        frame[13] == r->bssid[3] && frame[14] == r->bssid[4] && frame[15] == r->bssid[5])
        r->ap_signal_x8 += signal - r->ap_signal_x8 / 8;
    if (type == 0)
        r->st->rx_mgmt++;
    else if (type == 2)
        r->st->rx_data++;
    if (r->st->step == RTW_STEP_UP)
        wl_sta_rx(&r->sta, frame, length, signal, r->channel,
                  ((desc_w0 >> 20) & 7) != 0 && !(desc_w0 & BIT(27)));
}

/* Transfers out and none completing for TX_STALL_TICKS: the card no longer sends
 * (seen after a join, with beacons still coming in), so give the link up rather
 * than leave xnet on a card that cannot answer. */
#define TX_STALL_TICKS  60              /* ~2 s of 33 ms ticks */

static void tx_watchdog(RTW *r)
{
    if (!r->st->linked || !r->tx_inflight || r->st->tx_done != r->tx_seen) {
        r->tx_seen = r->st->tx_done;
        r->tx_quiet = 0;
        return;
    }
    if (++r->tx_quiet < TX_STALL_TICKS)
        return;
    r->tx_quiet = 0;
    r->st->tx_stalls++;
    r->st->stall_inflight = r->tx_inflight;
    r->st->stall_ep_busy = r->tx_ep_busy[RTW_EP_LOW];
    r->st->stall_open = r->tx_open[RTW_EP_LOW] == RTW_TX_COUNT ? 0 :
                        r->tx_open_frames[RTW_EP_LOW];
    wl_sta_lost(&r->sta);
}

/* Linked and no receive transfer for a while, or every one parked after errors:
 * the core's watch (usb_rx_watch) takes the transfers back after RX_KICK_TICKS,
 * which has always been enough when the host controller had stopped asking for
 * them under heavy sending (frames waiting in the card's FIFO, every transfer
 * queued, no error); after RX_STALL_TICKS the card is reset, and the core then
 * brings it up again and the station joins anew. */
#define RX_STALL_TICKS  60              /* ~2 s of 33 ms ticks; beacons come every 0.1 s */
#define RX_KICK_TICKS   4               /* ~0.13 s */

static void rx_watchdog(RTW *r)
{
    u32 i, busy, sending;

    if (!r->rx_watch.urbs)
        usb_rx_watch_init(&r->rx_watch, r->rx_urb, RTW_RX_COUNT, RX_KICK_TICKS,
                          RX_STALL_TICKS, &r->st->rx_kicks, &r->st->rx_kick_ok);
    /* the transfers are taken back only while sending, which is when the
     * controller stops (an idle link can go 130 ms without a receive transfer);
     * the reset after RX_STALL_TICKS stands either way */
    sending = r->tx_inflight || r->st->tx_frames != r->rx_watch_tx;
    r->rx_watch_tx = r->st->tx_frames;
    r->rx_watch.kick_ticks = sending ? RX_KICK_TICKS : 0;
    if (!usb_rx_watch(&r->rx_watch, r->st->linked,
                      r->rx_parked < RTW_RX_COUNT ? r->st->rx_transfers : r->rx_watch.seen))
        return;
    r->st->rx_stalls++;
    for (i = 0, busy = 0; i < RTW_RX_COUNT; i++)
        busy += r->rx_urb[i].Busy != 0;
    r->st->rxs_busy = busy;
    r->st->rxs_parked = r->rx_parked;
    r->st->rxs_errors = r->st->rx_errors;
    r->st->rxs_last_error = r->st->rx_last_error;
    r->st->rxs_rxpkt_num = rtw_read32(r, REG_RXPKT_NUM);
    r->st->rxs_rxdma_status = rtw_read32(r, REG_RXDMA_STATUS);
    r->st->rxs_txdma_status = rtw_read32(r, REG_TXDMA_STATUS);
    r->st->rxs_fifo_info = rtw_read32(r, 0x0230);
    UsbdResetDevice(r->dev->device);
}

#define DIG_TICKS       60              /* ~2 s of 33 ms ticks, rtw88's watchdog */

/* N1: about one boot in eight the card came up receiving nothing -- no scan result, no frame,
 * rx_transfers 0 -- and never joined; the receive watch (above) only watches a linked card.
 * ~30 s of that and the card is reset, as a stall is; at most every 2 minutes. */
#define DEAF_TICKS      900u            /* ~30 s of 33 ms ticks */
#define DEAF_GAP_TICKS  3600u

volatile u32 rtw_deaf_resets;
static u32 deaf_gap;

static void deaf_watch(RTW *r)
{
    if (deaf_gap)
        deaf_gap--;
    if (r->st->linked || r->st->rx_transfers) {
        r->deaf_ticks = 0;
        return;
    }
    if (++r->deaf_ticks < DEAF_TICKS || deaf_gap)
        return;
    r->deaf_ticks = 0;
    deaf_gap = DEAF_GAP_TICKS;
    rtw_deaf_resets++;
    UsbdResetDevice(r->dev->device);
}

static void rtw8822b_tick(NET_DEV *nd)
{
    RTW *r = (RTW *)nd->priv;

    if (!r || r->st->step != RTW_STEP_UP)
        return;
    deaf_watch(r);
    r->st->tx_inflight_now = r->tx_inflight;
    tx_watchdog(r);
    rx_watchdog(r);
    if (r->st->linked && ++r->dig_ticks >= DIG_TICKS) {
        r->dig_ticks = 0;
        rtw_dig(r);
    }
    r->link.mode->tick(&r->link);
    wl_sta_tick(&r->sta);
}

static u32 rtw8822b_command(NET_DEV *nd, u32 what, volatile u32 *args)
{
    RTW *r = (RTW *)nd->priv;

    if (!r || r->st->step != RTW_STEP_UP)
        return 0;
    switch (what) {
    case 30:                            /* scan: aic8800.c's numbering */
        args[5] = r->sta.card.scan(r->sta.card.context);
        return 1;
    case 31:                            /* join entry [1] of the scan table */
        args[5] = r->link.mode->join(&r->link, args[1]);
        return 1;
    case 32:                            /* leave, and stay off */
        args[5] = r->link.mode->leave(&r->link);
        return 1;
    default:
        return 0;
    }
}

static void rtw8822b_detached(NET_DEV *nd)
{
    RTW *r = (RTW *)nd->priv;

    if (!r)
        return;
    r->rx_running = 0;
    r->tx_ready = 0;
    if (r->link.mode)
        r->link.mode->gone(&r->link);
    r->in_use = 0;
    mem_sync();
}

static u32 rtw8822b_xmit(NET_DEV *nd, const u8 *frame, u32 length)
{
    RTW *r = (RTW *)nd->priv;

    return r && r->tx_ready ? wl_sta_xmit(&r->sta, frame, length) : 0;
}

static u32 rtw8822b_tx_space(NET_DEV *nd)
{
    RTW *r = (RTW *)nd->priv;

    return r && r->tx_ready && rtw_tx_space(r);
}

const XE_DRIVER xe_driver_rtw8822b = {
    .name = "rtw8822b",
    .kind = XE_KIND_WIFI,
    .match = rtw8822b_match,
    .start = rtw8822b_start,
    .detached = rtw8822b_detached,
    .xmit = rtw8822b_xmit,
    .tx_space = rtw8822b_tx_space,
    .command = rtw8822b_command,
    .tick = rtw8822b_tick,
};
