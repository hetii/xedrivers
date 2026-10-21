/* AIC8800D80 USB Wi-Fi: the driver the core sees (aic.h for the chip).
 *
 * Two phases, one device each time: a chip in boot ROM (8d80) gets its firmware and
 * is let go -- it leaves the bus and comes back as 8d81 with the Wi-Fi firmware
 * running, which is brought up and scans (aic_wifi.c). Not joining a network yet,
 * its NET_DEV has no link, so it never becomes the uplink.
 */

#include "aic.h"
#include "driver.h"

#define AIC_MAX 2

volatile AIC_STATS aic_stats[USB_MAX_DEVICES];

static AIC aic_inst[AIC_MAX];

/* The boot ROM by its product alone; the Wi-Fi firmware's composite device by its
 * vendor interface (the Bluetooth ones, class e0, are left), or by product alone
 * when only the device is known. */
static u32 aic8800_match(const u8 *device, const u8 *interface)
{
    u32 product = (u32)(device[10] | device[11] << 8);

    if ((u32)(device[8] | device[9] << 8) != AIC_VENDOR)
        return XE_MATCH_NONE;
    if (product == AIC_PRODUCT_D80)
        return XE_MATCH_VENDOR;
    if (product == AIC_PRODUCT_D81 && (!interface || interface[5] == 0xFF))
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

/* Which phase the chip is in: its device descriptor's product. */
static u32 product_of(AIC *a, u32 *product)
{
    static const u8 get_device[8] = { 0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 18, 0 };
    u32 status, actual = 0;

    if (!a->desc_buf)
        a->desc_buf = (u8 *)net_dma_scratch(AIC_DESC_SIZE);
    if (!a->desc_buf)
        return STATUS_UNSUCCESSFUL;
    status = usb_control(a->dev, get_device, a->desc_buf, 18, &actual, 1000);
    if (NT_SUCCESS(status) && actual < 12)
        status = STATUS_UNSUCCESSFUL;
    if (NT_SUCCESS(status))
        *product = (u32)(a->desc_buf[10] | a->desc_buf[11] << 8);
    return status;
}

static u32 aic8800_start(NET_DEV *nd)
{
    AIC *a = take_instance(nd);
    u32 step, status, product = 0;

    if (!a)
        return AIC_STEP_DMA;
    a->st->step = 0;
    status = product_of(a, &product);
    if (!NT_SUCCESS(status)) {
        a->st->step = AIC_STEP_DESCRIPTOR;
        a->st->step_status = status;
        return AIC_STEP_DESCRIPTOR;
    }
    a->st->product = product;
    if (product == AIC_PRODUCT_D81)
        return aic_wifi_start(a);
    if (product != AIC_PRODUCT_D80) {
        a->st->step = AIC_STEP_UNKNOWN_PRODUCT;
        return AIC_STEP_UNKNOWN_PRODUCT;
    }
    /* aicwf_parse_usb: the first bulk IN and OUT -- the boot ROM's 0x82 and 0x01. */
    a->ind = 0;
    step = aic_open(a, 0x01, 0x82);
    if (step) {
        a->st->step = step;
        return step;
    }
    step = aic_load_firmware(a);
    aic_close(a);
    return step;
}

/* xectl.py cmd 30: scan again; 31 [1]: join the network at that index of the scan
 * table; 32: leave it. */
#define CMD_SCAN       30u
#define CMD_JOIN       31u
#define CMD_DISCONNECT 32u

static u32 aic8800_command(NET_DEV *nd, u32 what, volatile u32 *args)
{
    AIC *a = (AIC *)nd->priv;

    if (!a || a->st->product != AIC_PRODUCT_D81)
        return 0;
    switch (what) {
    case CMD_SCAN:
        args[5] = aic_wifi_scan(a);
        return 1;
    case CMD_JOIN:
        a->auto_off = 0;
        args[5] = aic_wifi_join(a, args[1]);
        return 1;
    case CMD_DISCONNECT:
        a->auto_off = 1;                /* stay off until joined by command again */
        args[5] = aic_wifi_disconnect(a);
        return 1;
    default:
        return 0;
    }
}

static void aic8800_detached(NET_DEV *nd)
{
    AIC *a = (AIC *)nd->priv;

    if (!a)
        return;
    a->in_use = 0;
    a->running = 0;
    a->data_running = 0;
    a->joined = 0;
    mem_sync();
}

static void aic8800_tick(NET_DEV *nd)
{
    AIC *a = (AIC *)nd->priv;

    if (a && a->st->product == AIC_PRODUCT_D81 && a->st->step == AIC_STEP_WIFI_UP)
        aic_wifi_tick(a);
}

/* The handshake finishes where frames arrive, and the keys it settled have to be
 * installed on the worker: waking it means that happens in a moment rather than
 * at the next tick, which an access point waiting to send is glad of. */

static u32 aic8800_xmit(NET_DEV *nd, const u8 *frame, u32 length)
{
    AIC *a = (AIC *)nd->priv;

    return a ? aic_data_xmit(a, frame, length) : 0;
}

static u32 aic8800_tx_space(NET_DEV *nd)
{
    AIC *a = (AIC *)nd->priv;

    return a ? aic_data_tx_space(a) : 0;
}

const XE_DRIVER xe_driver_aic8800 = {
    .name = "aic8800",
    .kind = XE_KIND_WIFI,
    .match = aic8800_match,
    .start = aic8800_start,
    .detached = aic8800_detached,
    .xmit = aic8800_xmit,
    .tx_space = aic8800_tx_space,
    .command = aic8800_command,
    .tick = aic8800_tick,
};
