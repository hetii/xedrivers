/* AIC8800D80 USB Wi-Fi: the driver the core sees (aic.h for the chip).
 *
 * For now one phase: a chip in boot ROM (8d80) gets its firmware and is let go --
 * it leaves the bus and comes back as another product, which this driver does not
 * take yet. Its NET_DEV stays without a link, so it never becomes the uplink.
 */

#include "aic.h"
#include "driver.h"

#define AIC_MAX 2

volatile AIC_STATS aic_stats[USB_MAX_DEVICES];

static AIC aic_inst[AIC_MAX];

static u32 aic8800_match(const u8 *device, const u8 *interface)
{
    (void)interface;
    if ((u32)(device[8] | device[9] << 8) == AIC_VENDOR &&
        (u32)(device[10] | device[11] << 8) == AIC_PRODUCT_D80)
        return XE_MATCH_VENDOR;
    return XE_MATCH_NONE;
}

static AIC *take_instance(NET_DEV *nd)
{
    u32 i;

    for (i = 0; i < AIC_MAX; i++) {
        AIC *a = &aic_inst[i];

        if (a->in_use && a->nd != nd)
            continue;
        a->in_use = 1;
        a->nd = nd;
        a->dev = nd->usb;
        a->st = &aic_stats[nd->usb->index];
        nd->priv = a;
        return a;
    }
    return 0;
}

static u32 aic8800_start(NET_DEV *nd)
{
    AIC *a = take_instance(nd);
    u32 step;

    if (!a)
        return AIC_STEP_DMA;
    a->st->product = AIC_PRODUCT_D80;
    a->st->step = 0;
    step = aic_open(a);
    if (step) {
        a->st->step = step;
        return step;
    }
    step = aic_load_firmware(a);
    aic_close(a);
    return step;
}

static void aic8800_detached(NET_DEV *nd)
{
    AIC *a = (AIC *)nd->priv;

    if (!a)
        return;
    a->in_use = 0;
    a->running = 0;
    mem_sync();
}

static u32 aic8800_xmit(NET_DEV *nd, const u8 *frame, u32 length)
{
    (void)nd;
    (void)frame;
    (void)length;
    return 0;
}

static u32 aic8800_tx_space(NET_DEV *nd)
{
    (void)nd;
    return 0;
}

const XE_DRIVER xe_driver_aic8800 = {
    .name = "aic8800",
    .kind = XE_KIND_WIFI,
    .match = aic8800_match,
    .start = aic8800_start,
    .detached = aic8800_detached,
    .xmit = aic8800_xmit,
    .tx_space = aic8800_tx_space,
};
