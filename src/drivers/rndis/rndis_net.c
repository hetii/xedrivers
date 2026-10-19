/* The RNDIS driver: Ethernet over USB the way Android phones tether, and Windows
 * gadgets before them. Matched by the control interface's class triple (see
 * rndis_is_control), so any RNDIS function in the device's current configuration
 * is taken.
 *
 * Bring-up, from the worker thread, each control transfer waited for:
 *   GET_DESCRIPTOR configuration            find the control and data interfaces
 *   SET_CONFIGURATION, SET_INTERFACE data (only if its alt is not 0)
 *   INITIALIZE                               the device's largest transfer back
 *   QUERY 802_3_PERMANENT_ADDRESS            the address the host side uses
 *   SET CURRENT_PACKET_FILTER                promiscuous, like the CDC driver
 *   QUERY MEDIA_CONNECT_STATUS, LINK_SPEED   recorded; a phone says connected
 *   open bulk IN, bulk OUT, interrupt IN, receive ring queued for good
 * A control message goes out as SEND_ENCAPSULATED_COMMAND on the control
 * interface; its answer is fetched with GET_ENCAPSULATED_RESPONSE until it comes,
 * as Linux rndis_host does when the notification has not been seen. A keepalive
 * the device asks in between is answered; an indication is recorded.
 *
 * Data: one frame a transfer out, wrapped in the 44-byte PACKET_MSG header, with a
 * byte of padding when the transfer would be a whole number of packets (the
 * device reads MessageLength, not the transfer length); in, as many PACKET_MSGs
 * as the device packed into one transfer. After bring-up the interrupt endpoint
 * is drained and counted only: a link change the device indicates there is not
 * read (the control channel is thread-only), and a frame's arrival is what the
 * core takes as link up.
 */

#include "rndis.h"
#include "driver.h"
#include "net.h"
#include "usb.h"
#include "prof.h"

#define RX_COUNT    16
#define RX_SIZE     16384       /* what we tell the device it may send us at once */
#define TX_COUNT    32
#define TX_SIZE     2048        /* one wrapped frame, 44 + 1514 */
#define CTRL_SIZE   1088        /* a response of up to RNDIS_CONTROL_MAX */
#define INTR_SIZE   64
#define DMA_BYTES   (RX_COUNT * RX_SIZE + TX_COUNT * TX_SIZE + 2 * 2048)

#define RX_ERROR_LIMIT   64
#define INTR_ERROR_LIMIT 16
#define REPLY_POLLS      100    /* GET_ENCAPSULATED_RESPONSE tries, 5 ms apart */

enum {
    RNDIS_STEP_OK = 0,
    RNDIS_STEP_DMA,
    RNDIS_STEP_CONFIG_DESC,
    RNDIS_STEP_LAYOUT,
    RNDIS_STEP_SET_CONFIG,
    RNDIS_STEP_SET_INTERFACE,
    RNDIS_STEP_INIT,
    RNDIS_STEP_MAC,
    RNDIS_STEP_FILTER,
    RNDIS_STEP_ENDPOINTS,
};

/* Read by tests/xectl.py; field order is its STRUCTS["rndis_stats"]. */
typedef struct {
    u32 init_status;
    u32 max_packets;        /* MaxPacketsPerTransfer the device takes from us */
    u32 max_transfer;       /* MaxTransferSize the device takes from us */
    u32 alignment;
    u32 filter_status;
    u32 media_status;       /* MEDIA_CONNECT_STATUS: 0 connected */
    u32 link_speed;         /* LINK_SPEED, in 100 bit/s */
    u32 control_polls;      /* GET_ENCAPSULATED_RESPONSE answered with nothing */
    u32 keepalives;         /* asked by the device, answered */
    u32 indications;
    u32 last_indication;
    u32 rx_transfers;
    u32 rx_bad;             /* transfers that did not parse as PACKET_MSGs */
    u32 intr_events;
    u32 intr_errors;
    u32 tx_too_big;
} RNDIS_STATS;

volatile RNDIS_STATS rndis_stats;

static RNDIS_LAYOUT layout;
static void *ep_in;
static void *ep_out;
static void *ep_intr;

static URB rx_urb[RX_COUNT];
static u8 *rx_buf[RX_COUNT];
static u32 rx_consecutive_errors;

static URB tx_urb[TX_COUNT];
static u8 *tx_buf[TX_COUNT];
static u32 tx_next;
static u32 tx_inflight;
static u32 tx_limit;            /* the largest transfer the device accepts */
static KSPIN_LOCK tx_lock;

static URB intr_urb;
static u8 *intr_buf;
static u32 intr_consecutive_errors;

static u8 *ctrl_buf;
static u32 request_id;
static volatile u32 running;

static u32 fail(u32 step, u32 status)
{
    net_stats.step = step;
    net_stats.step_status = status;
    return step;
}

/* --- receive ----------------------------------------------------------------- */

static void rx_complete(URB *urb, u32 status);

static void rx_queue(URB *urb)
{
    usb_prepare(urb, ep_in, rx_complete, rx_buf[urb->Tag], RX_SIZE, 0);
    usb_submit(urb);
}

static void deliver(const u8 *frame, u32 length, void *context)
{
    (void)context;
    net_frame_in(frame, length);
}

static void rx_complete(URB *urb, u32 status)
{
    u32 actual = urb->Actual;
    PROF_START(t);

    if (!running || !usb.active) {
        urb->Busy = 0;
        return;
    }
    if (NT_SUCCESS(status)) {
        rx_consecutive_errors = 0;
        rndis_stats.rx_transfers++;
        if (rndis_unwrap(rx_buf[urb->Tag], actual, deliver, 0) < 0)
            rndis_stats.rx_bad++;
    } else {
        net_stats.rx_errors++;
        net_stats.rx_last_error = status;
        if (++rx_consecutive_errors > RX_ERROR_LIMIT) {
            net_stats.rx_stopped++;
            urb->Busy = 0;
            return;
        }
    }
    rx_queue(urb);
    PROF_END(rx_done, t, actual);
    (void)actual;
}

/* --- transmit ---------------------------------------------------------------- */

static void tx_complete(URB *urb, u32 status)
{
    u32 irql, length = urb->Length;
    PROF_START(t);

    irql = KfAcquireSpinLock(&tx_lock);
    if (NT_SUCCESS(status)) {
        net_stats.tx_frames++;
        net_add64(&net_stats.tx_bytes, &net_stats.tx_bytes_hi, length);
    } else {
        net_stats.tx_errors++;
        net_stats.tx_last_error = status;
    }
    tx_inflight--;
    net_stats.tx_inflight = tx_inflight;
    urb->Busy = 0;
    KfReleaseSpinLock(&tx_lock, irql);

    net_tx_done();
    PROF_END(tx_done, t, length);
    (void)length;
}

static u32 rndis_tx_space(void)
{
    return running && !tx_urb[tx_next].Busy;
}

static u32 rndis_xmit(const u8 *frame, u32 length)
{
    u32 irql, i, n;

    if (!running) {
        net_stats.tx_drops++;
        return 0;
    }
    /* Slots are taken in order and complete in order on one endpoint, so a busy
     * next slot means the ring is full. */
    irql = KfAcquireSpinLock(&tx_lock);
    i = tx_next;
    if (tx_urb[i].Busy) {
        KfReleaseSpinLock(&tx_lock, irql);
        net_stats.tx_drops++;
        return 0;
    }
    tx_urb[i].Busy = 1;
    tx_next = (i + 1) % TX_COUNT;
    tx_inflight++;
    net_stats.tx_inflight = tx_inflight;
    KfReleaseSpinLock(&tx_lock, irql);

    {
        PROF_START(t);
        n = rndis_wrap(tx_buf[i], TX_SIZE - 1, frame, length);
        PROF_END(copy, t, length);
    }
    if (!n || n > tx_limit) {
        rndis_stats.tx_too_big++;
        n = 0;
    }
    if (n && layout.out_max && (n % layout.out_max) == 0)
        tx_buf[i][n++] = 0;
    if (!n) {
        tx_complete(&tx_urb[i], STATUS_UNSUCCESSFUL);
        return 0;
    }
    usb_prepare(&tx_urb[i], ep_out, tx_complete, tx_buf[i], n, 0);
    usb_submit(&tx_urb[i]);
    return 1;
}

/* --- the interrupt endpoint ------------------------------------------------- */

static void intr_complete(URB *urb, u32 status);

static void intr_queue(void)
{
    usb_prepare(&intr_urb, ep_intr, intr_complete, intr_buf, layout.intr_max, 0);
    usb_submit(&intr_urb);
}

static void intr_complete(URB *urb, u32 status)
{
    if (!running || !usb.active) {
        urb->Busy = 0;
        return;
    }
    if (NT_SUCCESS(status)) {
        intr_consecutive_errors = 0;
        rndis_stats.intr_events++;
    } else {
        rndis_stats.intr_errors++;
        if (++intr_consecutive_errors > INTR_ERROR_LIMIT) {
            urb->Busy = 0;
            return;
        }
    }
    intr_queue();
}

/* --- control messages -------------------------------------------------------- */

static u32 control(u8 type, u8 request, u16 value, u16 index, u16 length,
                   u32 *actual)
{
    u8 setup[8];

    setup[0] = type;
    setup[1] = request;
    setup[2] = (u8)(value & 0xFF);
    setup[3] = (u8)(value >> 8);
    setup[4] = (u8)(index & 0xFF);
    setup[5] = (u8)(index >> 8);
    setup[6] = (u8)(length & 0xFF);
    setup[7] = (u8)(length >> 8);
    return usb_control(setup, ctrl_buf, length, actual, 1000);
}

/* Send the message in ctrl_buf (`length` bytes, RequestId `id`) and wait for its
 * completion, which is left in ctrl_buf; `reply` describes it. */
static u32 transact(u32 length, u32 id, RNDIS_REPLY *reply)
{
    u32 status, actual, poll;

    status = control(0x21, 0x00, 0, layout.comm_interface, (u16)length, 0);
    if (!NT_SUCCESS(status))
        return status;

    for (poll = 0; poll < REPLY_POLLS; poll++) {
        actual = 0;
        status = control(0xA1, 0x01, 0, layout.comm_interface, RNDIS_CONTROL_MAX,
                         &actual);
        if (!NT_SUCCESS(status) || !actual ||
            !rndis_parse_reply(ctrl_buf, actual, reply)) {
            rndis_stats.control_polls++;
            usb_sleep(5);
            continue;
        }
        if (reply->type == RNDIS_MSG_KEEPALIVE) {
            rndis_stats.keepalives++;
            control(0x21, 0x00, 0, layout.comm_interface,
                    (u16)rndis_build_keepalive_reply(ctrl_buf, reply->request), 0);
            continue;
        }
        if (reply->type == RNDIS_MSG_INDICATE) {
            rndis_stats.indications++;
            rndis_stats.last_indication = reply->status;
            continue;
        }
        if ((reply->type & RNDIS_MSG_COMPLETION) && reply->request == id)
            return reply->status == RNDIS_STATUS_SUCCESS ? STATUS_SUCCESS
                                                         : reply->status;
    }
    return STATUS_UNSUCCESSFUL;
}

static u32 query(u32 oid, RNDIS_REPLY *reply)
{
    u32 id = ++request_id;

    return transact(rndis_build_query(ctrl_buf, id, oid), id, reply);
}

static u32 le32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

/* --- bring-up ---------------------------------------------------------------- */

static u32 alloc_buffers(void)
{
    u32 i;

    if (!ctrl_buf) {
        ctrl_buf = (u8 *)net_dma_scratch(CTRL_SIZE);
        intr_buf = (u8 *)net_dma_scratch(INTR_SIZE);
    }
    if (!ctrl_buf || !intr_buf || !net_dma_begin(DMA_BYTES))
        return 0;
    for (i = 0; i < RX_COUNT; i++) {
        rx_buf[i] = (u8 *)net_dma_alloc(RX_SIZE);
        rx_urb[i].Tag = i;
    }
    for (i = 0; i < TX_COUNT; i++) {
        tx_buf[i] = (u8 *)net_dma_alloc(TX_SIZE);
        tx_urb[i].Tag = i;
        tx_urb[i].Busy = 0;
    }
    return rx_buf[RX_COUNT - 1] && tx_buf[TX_COUNT - 1];
}

static u32 rndis_start(void)
{
    u32 i, status, actual, total, id;
    RNDIS_REPLY reply;
    u8 filter[4];

    running = 0;
    rx_consecutive_errors = 0;
    intr_consecutive_errors = 0;
    ep_in = ep_out = ep_intr = 0;
    if (!alloc_buffers())
        return fail(RNDIS_STEP_DMA, 0);

    status = control(0x80, 0x06, 0x0200, 0, 9, &actual);
    if (!NT_SUCCESS(status) || actual < 9)
        return fail(RNDIS_STEP_CONFIG_DESC, status);
    total = (u32)(ctrl_buf[2] | ctrl_buf[3] << 8);
    if (total > CTRL_SIZE)
        return fail(RNDIS_STEP_CONFIG_DESC, total);
    status = control(0x80, 0x06, 0x0200, 0, (u16)total, &actual);
    if (!NT_SUCCESS(status) || !rndis_parse_config(ctrl_buf, actual, &layout))
        return fail(RNDIS_STEP_LAYOUT, status);

    status = control(0x00, 0x09, layout.config_value, 0, 0, 0);
    if (!NT_SUCCESS(status))
        return fail(RNDIS_STEP_SET_CONFIG, status);
    if (layout.data_alt) {
        status = control(0x01, 0x0B, layout.data_alt, layout.data_interface, 0, 0);
        if (!NT_SUCCESS(status))
            return fail(RNDIS_STEP_SET_INTERFACE, status);
    }

    id = ++request_id;
    status = transact(rndis_build_init(ctrl_buf, id, RX_SIZE), id, &reply);
    rndis_stats.init_status = status;
    if (!NT_SUCCESS(status))
        return fail(RNDIS_STEP_INIT, status);
    rndis_stats.max_packets = reply.max_packets;
    rndis_stats.max_transfer = reply.max_transfer;
    rndis_stats.alignment = reply.alignment;
    tx_limit = reply.max_transfer ? reply.max_transfer : TX_SIZE;

    status = query(OID_802_3_PERMANENT_ADDRESS, &reply);
    if (!NT_SUCCESS(status) || reply.info_length < 6)
        return fail(RNDIS_STEP_MAC, status);
    for (i = 0; i < 6; i++)
        net_mac[i] = reply.info[i];

    filter[0] = (u8)RNDIS_FILTER_DEFAULT;
    filter[1] = filter[2] = filter[3] = 0;
    id = ++request_id;
    status = transact(rndis_build_set(ctrl_buf, id, OID_GEN_CURRENT_PACKET_FILTER,
                                      filter, 4), id, &reply);
    rndis_stats.filter_status = status;
    if (!NT_SUCCESS(status))
        return fail(RNDIS_STEP_FILTER, status);

    /* Optional: a device that does not answer these still carries frames. */
    if (NT_SUCCESS(query(OID_GEN_MEDIA_CONNECT_STATUS, &reply)) &&
        reply.info_length >= 4) {
        rndis_stats.media_status = le32(reply.info);
        net_link_report(rndis_stats.media_status == 0);
    }
    if (NT_SUCCESS(query(OID_GEN_LINK_SPEED, &reply)) && reply.info_length >= 4) {
        rndis_stats.link_speed = le32(reply.info);
        net_speed_report(rndis_stats.link_speed / 10000u);
    }

    ep_in = usb_open(USB_TYPE_BULK, layout.ep_in, layout.in_max, 0);
    ep_out = usb_open(USB_TYPE_BULK, layout.ep_out, layout.out_max, 0);
    if (layout.ep_intr)
        ep_intr = usb_open(USB_TYPE_INTERRUPT, layout.ep_intr, layout.intr_max,
                           layout.intr_interval);
    if (!ep_in || !ep_out)
        return fail(RNDIS_STEP_ENDPOINTS, usb_stats.last_open_status);

    tx_next = 0;
    tx_inflight = 0;
    running = 1;
    mem_sync();
    for (i = 0; i < RX_COUNT; i++)
        rx_queue(&rx_urb[i]);
    if (ep_intr)
        intr_queue();
    net_stats.step = RNDIS_STEP_OK;
    return 0;
}

static void rndis_detached(void)
{
    running = 0;
    mem_sync();
}

static u32 rndis_match(const u8 *device, const u8 *interface)
{
    (void)device;
    if (interface && rndis_is_control(interface[5], interface[6], interface[7]))
        return XE_MATCH_CLASS;
    return XE_MATCH_NONE;
}

const XE_DRIVER xe_driver_rndis = {
    "rndis", rndis_match, rndis_start, rndis_detached, rndis_xmit, rndis_tx_space, 0
};
