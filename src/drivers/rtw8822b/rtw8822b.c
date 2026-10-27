/* Realtek RTL8822BU: the driver the core sees (rtw.h for the chip).
 *
 * Brought up as rtw88's probe, power_on and core_start do it: the MAC powered,
 * the firmware downloaded and running, the efuse read for the card's address,
 * the MAC, baseband and radio set up, a station's port on that address, and
 * receive running. It then listens on each channel in turn for beacons
 * (wireless.c's table); it does not join anything yet, so its NET_DEV has no link
 * and never becomes the uplink.
 */

#include "rtw.h"
#include "rtw8822b_defs.h"
#include "driver.h"
#include "wireless.h"

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
#define DWELL_TICKS         5           /* ~150 ms on a channel */
#define SCAN_EVERY_TICKS    900         /* a scan every ~30 s */

static const u8 scan_channels[] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
    36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128, 132,
    136, 140, 144, 149, 153, 157, 161, 165,
};

volatile RTW_STATS rtw_stats[USB_MAX_DEVICES];

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
                       (RTW_RX_COUNT + 2) * 2048))
        return fail(r, RTW_STEP_DMA, 0);
    r->reg_buf = (u8 *)net_dma_alloc(nd, REG_BUF_SIZE);
    r->bulk_buf = (u8 *)net_dma_alloc(nd, BULK_BUF_SIZE);
    for (step = 0; step < RTW_RX_COUNT; step++)
        r->rx_buf[step] = (u8 *)net_dma_alloc(nd, RTW_RX_SIZE);
    if (!r->fw)
        r->fw = (u8 *)MmAllocatePhysicalMemory(2, RTW_FW_MAX, 4);
    if (!r->reg_buf || !r->bulk_buf || !r->fw || !r->rx_buf[RTW_RX_COUNT - 1])
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
    rtw_rx_start(r);
    r->scan_index = 0;
    r->scan_ticks = 0;
    r->scan_wait = 0;
    r->st->step = RTW_STEP_UP;
    return 0;
}

static u32 freq_of(u32 channel)
{
    return channel <= 14 ? 2407 + 5 * channel : 5000 + 5 * channel;
}

/* A beacon or probe response goes to the scan table; nothing else is used yet. */
void rtw_rx_frame(RTW *r, const u8 *frame, u32 length, s32 signal, u32 enc)
{
    u32 type = (frame[0] >> 2) & 3, subtype = frame[0] >> 4;

    (void)enc;
    r->st->last_signal = signal;
    if (type == 0) {
        r->st->rx_mgmt++;
        if (subtype == 8 || subtype == 5) {
            r->st->rx_beacons++;
            wl_scan_result(frame, length, freq_of(r->channel), signal);
        }
    } else if (type == 2) {
        r->st->rx_data++;
    }
}

/* A passive scan: each channel in turn for DWELL_TICKS, then back to the first. */
static void rtw8822b_tick(NET_DEV *nd)
{
    RTW *r = (RTW *)nd->priv;

    if (!r || r->st->step != RTW_STEP_UP)
        return;
    if (r->scan_wait) {
        r->scan_wait--;
        return;
    }
    if (r->scan_index == 0 && r->scan_ticks == 0) {
        r->st->scans++;
        wl_scan_begin();
        rtw_set_channel(r, scan_channels[r->scan_index], 0, 0);
    }
    if (++r->scan_ticks < DWELL_TICKS)
        return;
    r->scan_ticks = 0;
    if (++r->scan_index < sizeof(scan_channels)) {
        rtw_set_channel(r, scan_channels[r->scan_index], 0, 0);
        return;
    }
    wl_scan_end(0);
    r->scan_index = 0;
    r->scan_wait = SCAN_EVERY_TICKS;
}

static void rtw8822b_detached(NET_DEV *nd)
{
    RTW *r = (RTW *)nd->priv;

    if (!r)
        return;
    r->rx_running = 0;
    r->in_use = 0;
    mem_sync();
}

static u32 rtw8822b_xmit(NET_DEV *nd, const u8 *frame, u32 length)
{
    (void)nd;
    (void)frame;
    (void)length;
    return 0;
}

static u32 rtw8822b_tx_space(NET_DEV *nd)
{
    (void)nd;
    return 0;
}

const XE_DRIVER xe_driver_rtw8822b = {
    .name = "rtw8822b",
    .kind = XE_KIND_WIFI,
    .match = rtw8822b_match,
    .start = rtw8822b_start,
    .detached = rtw8822b_detached,
    .xmit = rtw8822b_xmit,
    .tx_space = rtw8822b_tx_space,
    .tick = rtw8822b_tick,
};
