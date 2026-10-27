/* Realtek RTL8822BU: the driver the core sees (rtw.h for the chip).
 *
 * Brought up as rtw88's probe and power_on do it (rtw_chip_efuse_enable,
 * rtw_power_on): the MAC powered, the firmware downloaded and running, the efuse
 * read for the card's address. It does not join anything yet, so its NET_DEV has
 * no link and never becomes the uplink.
 */

#include "rtw.h"
#include "driver.h"

#define RTW_MAX             2
#define REG_BUF_SIZE        64
#define BULK_BUF_SIZE       8192        /* a firmware block, 4 KB and its descriptor */
#define C2H_HW_FEATURE_DUMP 0xFD

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
    if (!net_dma_begin(nd, REG_BUF_SIZE + BULK_BUF_SIZE + 4096))
        return fail(r, RTW_STEP_DMA, 0);
    r->reg_buf = (u8 *)net_dma_alloc(nd, REG_BUF_SIZE);
    r->bulk_buf = (u8 *)net_dma_alloc(nd, BULK_BUF_SIZE);
    if (!r->fw)
        r->fw = (u8 *)MmAllocatePhysicalMemory(2, RTW_FW_MAX, 4);
    if (!r->reg_buf || !r->bulk_buf || !r->fw)
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

    r->st->step = RTW_STEP_UP;
    return 0;
}

static void rtw8822b_detached(NET_DEV *nd)
{
    RTW *r = (RTW *)nd->priv;

    if (!r)
        return;
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
};
