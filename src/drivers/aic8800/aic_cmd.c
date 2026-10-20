/* The AIC8800 message channel: a command over bulk OUT, its confirm over bulk IN
 * (aic.h for the layout). One command at a time, from the worker thread, which
 * waits for the confirm; the receive ring stays queued meanwhile and its completion
 * (DISPATCH_LEVEL) hands the confirm over and wakes the worker. */

#include "aic.h"

#define CFM_TIMEOUT_MS   2000   /* RWNX_80211_CMD_TIMEOUT_MS */
#define TX_TIMEOUT_MS    1000
#define RX_ERROR_LIMIT   64

static void rx_complete(URB *urb, u32 status);

static void rx_queue(AIC *a, URB *urb)
{
    usb_prepare(urb, a->dev, a->ep_in, rx_complete, a->rx_buf[urb->Tag], AIC_RX_SIZE, 0);
    usb_submit(urb);
}

/* One message of a transfer. */
static void message(AIC *a, const u8 *m, u32 length)
{
    u32 id, params;

    if (length < AIC_CFM_HEADER - 4)
        return;
    id = (u32)(m[0] | m[1] << 8);
    params = (u32)(m[6] | m[7] << 8);
    a->st->rx_messages++;
    a->st->rx_last_id = id;
    if (id != a->wait_id || a->cfm_done) {
        a->st->rx_unexpected++;
        return;
    }
    if (params > AIC_CFM_MAX)
        params = AIC_CFM_MAX;
    if (params > length - (AIC_CFM_HEADER - 4))
        params = length - (AIC_CFM_HEADER - 4);
    for (a->cfm_len = 0; a->cfm_len < params; a->cfm_len++)
        a->cfm[a->cfm_len] = m[AIC_CFM_HEADER - 4 + a->cfm_len];
    a->st->last_cfm_id = id;
    mem_sync();
    a->cfm_done = 1;
    usb_wake();
}

/* aicwf_process_rxframes: packets back to back, each a 4-byte header with its length,
 * padded to 4; a configuration packet's length counts what follows the header, a
 * data packet's leaves out the 60-byte descriptor too. A zero length ends it. */
static void parse(AIC *a, const u8 *p, u32 total)
{
    u32 at = 0, length, size;

    while (at + 4 <= total) {
        length = (u32)(p[at] | p[at + 1] << 8);
        if (!length)
            break;
        if (p[at + 2] & AIC_TYPE_CFG) {
            if ((p[at + 2] & 0x7F) == AIC_TYPE_CMD_RSP && at + 4 + length <= total)
                message(a, p + at + 4, length);
            size = ((length + 3) & ~3u) + 4;
        } else {
            a->st->rx_data++;
            size = (length + AIC_RX_HW_HEADER + 3) & ~3u;
        }
        at += size;
    }
}

static void rx_complete(URB *urb, u32 status)
{
    AIC *a = (AIC *)urb->Owner;
    u32 i;

    if (!a->running || !a->dev->active)
        return;
    if (!NT_SUCCESS(status)) {
        a->st->rx_errors++;
        a->st->rx_last_error = status;
        if (++a->rx_consecutive_errors >= RX_ERROR_LIMIT)
            return;                             /* the device is going */
        rx_queue(a, urb);
        return;
    }
    a->rx_consecutive_errors = 0;
    if (!a->st->rx_transfers) {
        const u8 *b = a->rx_buf[urb->Tag];

        for (i = 0; i < 8 && i * 4 + 4 <= urb->Actual; i++)
            a->st->rx_first[i] = (u32)(b[i * 4] << 24 | b[i * 4 + 1] << 16 |
                                       b[i * 4 + 2] << 8 | b[i * 4 + 3]);
    }
    a->st->rx_transfers++;
    parse(a, a->rx_buf[urb->Tag], urb->Actual);
    rx_queue(a, urb);
}

static void tx_complete(URB *urb, u32 status)
{
    AIC *a = (AIC *)urb->Owner;

    a->tx_status = status;
    mem_sync();
    a->tx_done = 1;
    usb_wake();
}

u32 aic_open(AIC *a)
{
    u32 i;

    if (!a->tx_buf) {
        a->tx_buf = (u8 *)net_dma_scratch(AIC_TX_SIZE);
        a->file_buf = (u8 *)net_dma_scratch(AIC_BLOCK);
    }
    if (!a->tx_buf || !a->file_buf ||
        !net_dma_begin(a->nd, AIC_RX_COUNT * AIC_RX_SIZE + 2048))
        return AIC_STEP_DMA;
    for (i = 0; i < AIC_RX_COUNT; i++) {
        a->rx_buf[i] = (u8 *)net_dma_alloc(a->nd, AIC_RX_SIZE);
        if (!a->rx_buf[i])
            return AIC_STEP_DMA;
        a->rx_urb[i].Tag = i;
        a->rx_urb[i].Owner = a;
    }
    a->tx_urb.Owner = a;

    /* aicwf_parse_usb: the first bulk IN and the first bulk OUT. The boot ROM's are
     * 0x82 and 0x01, 512 bytes, whatever the descriptor would say. */
    a->ep_in = usb_open(a->dev, USB_TYPE_BULK, 0x82, 512, 0);
    a->ep_out = usb_open(a->dev, USB_TYPE_BULK, 0x01, 512, 0);
    if (!a->ep_in || !a->ep_out)
        return AIC_STEP_ENDPOINTS;

    a->rx_consecutive_errors = 0;
    a->wait_id = 0;
    a->running = 1;
    mem_sync();
    for (i = 0; i < AIC_RX_COUNT; i++)
        rx_queue(a, &a->rx_urb[i]);
    return 0;
}

void aic_close(AIC *a)
{
    a->running = 0;
    mem_sync();
}

/* Send message `id` with `length` bytes of parameters (already little-endian);
 * wait for `cfm_id` if not 0, its parameters then in a->cfm. Returns an NTSTATUS. */
u32 aic_send(AIC *a, u32 id, const void *params, u32 length, u32 cfm_id)
{
    const u8 *in = (const u8 *)params;
    u8 *b = a->tx_buf;
    u32 i, total = AIC_MSG_HEADER + length, waited;

    if (!a->running || !a->dev->active || total > AIC_TX_SIZE)
        return STATUS_UNSUCCESSFUL;
    b[0] = (u8)(total - 4);
    b[1] = (u8)(((total - 4) >> 8) & 0x0F);
    b[2] = AIC_TYPE_CMD_RSP;
    b[3] = 0;
    for (i = 4; i < 8; i++)
        b[i] = 0;
    b[8] = (u8)id;
    b[9] = (u8)(id >> 8);
    b[10] = (u8)(id >> 10);             /* destination: the message's task */
    b[11] = 0;
    b[12] = AIC_DRV_TASK;
    b[13] = 0;
    b[14] = (u8)length;
    b[15] = (u8)(length >> 8);
    for (i = 0; i < length; i++)
        b[AIC_MSG_HEADER + i] = in[i];

    a->st->commands++;
    a->cfm_done = 0;
    a->wait_id = cfm_id;
    a->tx_done = 0;
    mem_sync();
    usb_prepare(&a->tx_urb, a->dev, a->ep_out, tx_complete, b, total, 0);
    usb_submit(&a->tx_urb);

    for (waited = 0; !a->tx_done; waited++) {
        if (waited >= TX_TIMEOUT_MS || !a->dev->active) {
            a->st->timeouts++;
            return STATUS_UNSUCCESSFUL;
        }
        usb_wait(1);
    }
    if (!NT_SUCCESS(a->tx_status)) {
        a->st->tx_errors++;
        a->st->tx_last_error = a->tx_status;
        return a->tx_status;
    }
    if (!cfm_id)
        return STATUS_SUCCESS;
    for (waited = 0; !a->cfm_done; waited++) {
        if (waited >= CFM_TIMEOUT_MS || !a->dev->active) {
            a->wait_id = 0;
            a->st->timeouts++;
            return STATUS_UNSUCCESSFUL;
        }
        usb_wait(1);
    }
    a->wait_id = 0;
    return STATUS_SUCCESS;
}

u32 aic_mem_read(AIC *a, u32 address, u32 *value)
{
    u8 p[4];
    u32 status;

    aic_put32(p, address);
    status = aic_send(a, DBG_MEM_READ_REQ, p, 4, DBG_MEM_READ_CFM);
    if (NT_SUCCESS(status) && a->cfm_len < 8)
        status = STATUS_UNSUCCESSFUL;
    if (NT_SUCCESS(status))
        *value = aic_le32(a->cfm + 4);      /* memaddr, memdata */
    return status;
}

u32 aic_mem_write(AIC *a, u32 address, u32 value)
{
    u8 p[8];

    aic_put32(p, address);
    aic_put32(p + 4, value);
    return aic_send(a, DBG_MEM_WRITE_REQ, p, 8, DBG_MEM_WRITE_CFM);
}

u32 aic_mem_mask_write(AIC *a, u32 address, u32 mask, u32 value)
{
    u8 p[12];

    aic_put32(p, address);
    aic_put32(p + 4, mask);
    aic_put32(p + 8, value);
    return aic_send(a, DBG_MEM_MASK_WRITE_REQ, p, 12, DBG_MEM_MASK_WRITE_CFM);
}

/* dbg_mem_block_write_req is always sent whole, 1024 data bytes whatever memsize
 * says. The data are file bytes, already in the chip's order. */
u32 aic_block_write(AIC *a, u32 address, const u8 *data, u32 length)
{
    u8 *b = a->tx_buf + AIC_MSG_HEADER;
    u32 i, status;

    if (length > AIC_BLOCK)
        return STATUS_UNSUCCESSFUL;
    /* Built in place: aic_send copies `params` onto itself, harmless. */
    aic_put32(b, address);
    aic_put32(b + 4, length);
    for (i = 0; i < length; i++)
        b[8 + i] = data[i];
    for (; i < AIC_BLOCK; i++)
        b[8 + i] = 0;
    status = aic_send(a, DBG_MEM_BLOCK_WRITE_REQ, b, 8 + AIC_BLOCK,
                      DBG_MEM_BLOCK_WRITE_CFM);
    return status;
}

u32 aic_start_app(AIC *a, u32 address, u32 type)
{
    u8 p[8];

    aic_put32(p, address);
    aic_put32(p + 4, type);
    return aic_send(a, DBG_START_APP_REQ, p, 8, 0);
}
