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
 * Data: out, frames wrapped in the 44-byte PACKET_MSG header and packed into one
 * transfer up to the device's MaxPacketsPerTransfer and MaxTransferSize, each
 * message padded to its PacketAlignmentFactor; with fewer than `rndis_tx_eager`
 * transfers in flight the open one goes at once (latency), otherwise frames are
 * added until it fills or a transfer completes (throughput) -- as the CDC driver's
 * NCM does. A byte of padding when a transfer would be a whole number of packets
 * (the device reads MessageLength, not the transfer length). In, as many
 * PACKET_MSGs as the device packed into one transfer.
 *
 * Measured on the console to a Xiaomi phone (MaxPacketsPerTransfer 15, 23700
 * bytes, alignment 16), UDP of 1472-byte payloads to the phone's broadcast
 * address (tests/phoneblast.py): one frame a transfer 21.2 MB/s, 13588 frames/s;
 * packed 24.5 MB/s, 15600 frames/s, about eleven a transfer, whatever
 * rndis_tx_eager (1, 2, 4, 8). That is what the phone's USB side accepted; what
 * it did with the frames after is not seen from here. After bring-up the interrupt endpoint
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
#define RX_SIZE     16384       /* each receive buffer; what we allow is rndis_rx_max */
#define TX_COUNT    16
#define TX_SIZE     24576       /* up to 15 wrapped frames, as a phone takes them */
#define CTRL_SIZE   1088        /* a response of up to RNDIS_CONTROL_MAX */
#define INTR_SIZE   64
#define DMA_BYTES   (RX_COUNT * RX_SIZE + TX_COUNT * TX_SIZE + 2 * 2048)

#define RX_ERROR_LIMIT   64
#define INTR_ERROR_LIMIT 16
#define REPLY_POLLS      100    /* GET_ENCAPSULATED_RESPONSE tries, 5 ms apart */
#define NONE             0xFFFFFFFFu

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
    u32 tx_transfers;
} RNDIS_STATS;

#define RNDIS_MAX 2         /* phones at once */

volatile RNDIS_STATS rndis_stats[RNDIS_MAX];

/* Below this many transfers in flight a frame goes out at once; at or above it,
 * frames are packed; an idle pipe always gets the open transfer at once, so 0 and
 * 1 both mean "pack whenever anything is in flight".
 * Tunable at run time (xectl.py set rndis_tx_eager N). */
volatile u32 rndis_tx_eager = 2;

/* The largest transfer we tell the device it may send us (INITIALIZE's
 * MaxTransferSize), read at every bring-up. 2048 is what Linux rndis_host asks for
 * -- one frame a transfer -- and so what Android phones are tested against. Offered
 * 16384, the Xiaomi packed received frames together and held each block until it
 * filled or its own timer ran out: a TCP upload from the phone, whose window never
 * filled a block, crawled at 75-90 KB/s in a steady rhythm, while downloads (small
 * ACKs from the phone) were not hurt. Up to RX_SIZE. */
volatile u32 rndis_rx_max = 2048;

/* One device's worth of state: RNDIS_MAX of them, so two phones can be attached
 * at once (the one that tethered first is the uplink). Completions find theirs
 * through the request's Owner. */
typedef struct RNDIS {
    u32 in_use;
    NET_DEV *nd;
    USB_DEV *dev;
    volatile RNDIS_STATS *st;
    RNDIS_LAYOUT layout;
    void *ep_in;
    void *ep_out;
    void *ep_intr;
    URB rx_urb[RX_COUNT];
    u8 *rx_buf[RX_COUNT];
    u32 rx_consecutive_errors;
    URB tx_urb[TX_COUNT];
    u8 *tx_buf[TX_COUNT];
    u32 tx_len[TX_COUNT];
    u32 tx_frames_in[TX_COUNT];
    u32 tx_next;
    u32 tx_inflight;
    u32 tx_limit;               /* the largest transfer the device accepts */
    u32 tx_max_packets;         /* and the most messages in one */
    u32 tx_align;               /* each message padded to this */
    KSPIN_LOCK tx_lock;
    u32 open_slot;              /* the transfer being filled */
    u32 open_len;
    u32 open_count;
    URB intr_urb;
    u8 *intr_buf;
    u32 intr_consecutive_errors;
    u8 *ctrl_buf;
    u32 request_id;
    volatile u32 running;
} RNDIS;

static RNDIS rndis_inst[RNDIS_MAX];

static u32 fail(RNDIS *r, u32 step, u32 status)
{
    r->nd->st->step = step;
    r->nd->st->step_status = status;
    return step;
}

/* --- receive ----------------------------------------------------------------- */

static void rx_complete(URB *urb, u32 status);

static void rx_queue(URB *urb)
{
    RNDIS *r = (RNDIS *)urb->Owner;
    usb_prepare(urb, r->dev, r->ep_in, rx_complete, r->rx_buf[urb->Tag], RX_SIZE, 0);
    usb_submit(urb);
}

static void deliver(const u8 *frame, u32 length, void *context)
{
    RNDIS *r = (RNDIS *)context;

    net_frame_in(r->nd, frame, length);
}

static void rx_complete(URB *urb, u32 status)
{
    RNDIS *r = (RNDIS *)urb->Owner;
    u32 actual = urb->Actual;
    PROF_START(t);

    if (!r->running || !r->dev->active) {
        urb->Busy = 0;
        return;
    }
    if (NT_SUCCESS(status)) {
        r->rx_consecutive_errors = 0;
        r->st->rx_transfers++;
        if (rndis_unwrap(r->rx_buf[urb->Tag], actual, deliver, r) < 0)
            r->st->rx_bad++;
    } else {
        r->nd->st->rx_errors++;
        r->nd->st->rx_last_error = status;
        if (++r->rx_consecutive_errors > RX_ERROR_LIMIT) {
            r->nd->st->rx_stopped++;
            urb->Busy = 0;
            return;
        }
    }
    rx_queue(urb);
    PROF_END(rx_done, t, actual);
    (void)actual;
}

/* --- transmit ---------------------------------------------------------------- */

static void tx_complete(URB *urb, u32 status);

static void tx_send(RNDIS *r, u32 i)
{
    if (i == NONE)
        return;
    usb_prepare(&r->tx_urb[i], r->dev, r->ep_out, tx_complete, r->tx_buf[i], r->tx_len[i], 0);
    usb_submit(&r->tx_urb[i]);
}

/* Close the open transfer and return its slot for sending, or NONE. Lock held. */
static u32 close_locked(RNDIS *r)
{
    u32 i = r->open_slot, n = r->open_len;

    if (i == NONE || !r->open_count)
        return NONE;
    if (r->layout.out_max && (n % r->layout.out_max) == 0)
        r->tx_buf[i][n++] = 0;
    r->tx_len[i] = n;
    r->tx_frames_in[i] = r->open_count;
    r->open_slot = NONE;
    r->tx_inflight++;
    r->nd->st->tx_inflight = r->tx_inflight;
    r->st->tx_transfers++;
    return i;
}

/* Take the next slot and start a transfer in it; 0 if the ring is full. Lock held. */
static u32 open_locked(RNDIS *r)
{
    u32 i = r->tx_next;

    if (r->tx_urb[i].Busy)
        return 0;
    r->tx_urb[i].Busy = 1;
    r->tx_next = (i + 1) % TX_COUNT;
    r->open_slot = i;
    r->open_len = 0;
    r->open_count = 0;
    return 1;
}

/* Append one wrapped frame to the open transfer; 0 if it does not fit. Lock held. */
static u32 add_locked(RNDIS *r, const u8 *frame, u32 length)
{
    u32 room = r->tx_limit < TX_SIZE - 1 ? r->tx_limit : TX_SIZE - 1, n;

    if (r->open_count >= r->tx_max_packets || r->open_len >= room)
        return 0;
    PROF_START(t);
    n = rndis_wrap_aligned(r->tx_buf[r->open_slot] + r->open_len, room - r->open_len, frame,
                           length, r->tx_align);
    PROF_END(copy, t, length);
    if (!n)
        return 0;
    r->open_len += n;
    r->open_count++;
    return 1;
}

static void tx_complete(URB *urb, u32 status)
{
    RNDIS *r = (RNDIS *)urb->Owner;
    u32 irql, send = NONE, i = urb->Tag, length = urb->Length;
    PROF_START(t);

    irql = KfAcquireSpinLock(&r->tx_lock);
    if (NT_SUCCESS(status)) {
        r->nd->st->tx_frames += r->tx_frames_in[i];
        net_add64(&r->nd->st->tx_bytes, &r->nd->st->tx_bytes_hi, length);
    } else {
        r->nd->st->tx_errors++;
        r->nd->st->tx_last_error = status;
    }
    r->tx_inflight--;
    r->nd->st->tx_inflight = r->tx_inflight;
    urb->Busy = 0;
    if (!r->tx_inflight || r->tx_inflight < rndis_tx_eager)
        send = close_locked(r);
    KfReleaseSpinLock(&r->tx_lock, irql);

    tx_send(r, send);
    net_tx_done(r->nd);
    PROF_END(tx_done, t, length);
    (void)length;
}

/* Room for one more full-size frame: in the open transfer, or a slot to open. */
static u32 rndis_tx_space(NET_DEV *nd)
{
    RNDIS *r = (RNDIS *)nd->priv;
    u32 room = r->tx_limit < TX_SIZE - 1 ? r->tx_limit : TX_SIZE - 1;

    if (!r->running)
        return 0;
    if (r->open_slot != NONE && r->open_count < r->tx_max_packets &&
        r->open_len + RNDIS_PACKET_HEADER + 1518 + r->tx_align <= room)
        return 1;
    return !r->tx_urb[r->tx_next].Busy;
}

/* Every submit happens after the lock is released: a transfer that completed at
 * once would re-enter tx_complete and deadlock on it. */
static u32 rndis_xmit(NET_DEV *nd, const u8 *frame, u32 length)
{
    RNDIS *r = (RNDIS *)nd->priv;
    u32 irql, full = NONE, now = NONE, queued = 0;

    if (!r->running) {
        r->nd->st->tx_drops++;
        return 0;
    }
    irql = KfAcquireSpinLock(&r->tx_lock);
    if (r->open_slot == NONE && !open_locked(r))
        goto out;
    if (!add_locked(r, frame, length)) {
        if (!r->open_count) {                  /* does not fit even alone */
            r->st->tx_too_big++;
            goto out;
        }
        full = close_locked(r);
        if (!open_locked(r) || !add_locked(r, frame, length))
            goto out;
    }
    queued = 1;
    if (!r->tx_inflight || r->tx_inflight < rndis_tx_eager)
        now = close_locked(r);
out:
    KfReleaseSpinLock(&r->tx_lock, irql);

    tx_send(r, full);
    tx_send(r, now);
    if (!queued)
        r->nd->st->tx_drops++;
    return queued;
}

/* --- the interrupt endpoint ------------------------------------------------- */

static void intr_complete(URB *urb, u32 status);

static void intr_queue(RNDIS *r)
{
    usb_prepare(&r->intr_urb, r->dev, r->ep_intr, intr_complete, r->intr_buf, r->layout.intr_max, 0);
    usb_submit(&r->intr_urb);
}

static void intr_complete(URB *urb, u32 status)
{
    RNDIS *r = (RNDIS *)urb->Owner;
    if (!r->running || !r->dev->active) {
        urb->Busy = 0;
        return;
    }
    if (NT_SUCCESS(status)) {
        r->intr_consecutive_errors = 0;
        r->st->intr_events++;
    } else {
        r->st->intr_errors++;
        if (++r->intr_consecutive_errors > INTR_ERROR_LIMIT) {
            urb->Busy = 0;
            return;
        }
    }
    intr_queue(r);
}

/* --- control messages -------------------------------------------------------- */

static u32 control(RNDIS *r, u8 type, u8 request, u16 value, u16 index, u16 length,
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
    return usb_control(r->dev, setup, r->ctrl_buf, length, actual, 1000);
}

/* Send the message in ctrl_buf (`length` bytes, RequestId `id`) and wait for its
 * completion, which is left in ctrl_buf; `reply` describes it. */
static u32 transact(RNDIS *r, u32 length, u32 id, RNDIS_REPLY *reply)
{
    u32 status, actual, poll;

    status = control(r, 0x21, 0x00, 0, r->layout.comm_interface, (u16)length, 0);
    if (!NT_SUCCESS(status))
        return status;

    for (poll = 0; poll < REPLY_POLLS; poll++) {
        actual = 0;
        status = control(r, 0xA1, 0x01, 0, r->layout.comm_interface, RNDIS_CONTROL_MAX,
                         &actual);
        if (!NT_SUCCESS(status) || !actual ||
            !rndis_parse_reply(r->ctrl_buf, actual, reply)) {
            r->st->control_polls++;
            usb_sleep(5);
            continue;
        }
        if (reply->type == RNDIS_MSG_KEEPALIVE) {
            r->st->keepalives++;
            control(r, 0x21, 0x00, 0, r->layout.comm_interface,
                    (u16)rndis_build_keepalive_reply(r->ctrl_buf, reply->request), 0);
            continue;
        }
        if (reply->type == RNDIS_MSG_INDICATE) {
            r->st->indications++;
            r->st->last_indication = reply->status;
            continue;
        }
        if ((reply->type & RNDIS_MSG_COMPLETION) && reply->request == id)
            return reply->status == RNDIS_STATUS_SUCCESS ? STATUS_SUCCESS
                                                         : reply->status;
    }
    return STATUS_UNSUCCESSFUL;
}

static u32 query(RNDIS *r, u32 oid, RNDIS_REPLY *reply)
{
    u32 id = ++r->request_id;

    return transact(r, rndis_build_query(r->ctrl_buf, id, oid), id, reply);
}

static u32 le32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

/* --- bring-up ---------------------------------------------------------------- */

static u32 alloc_buffers(RNDIS *r)
{
    u32 i;

    if (!r->ctrl_buf) {
        r->ctrl_buf = (u8 *)net_dma_scratch(CTRL_SIZE);
        r->intr_buf = (u8 *)net_dma_scratch(INTR_SIZE);
    }
    if (!r->ctrl_buf || !r->intr_buf || !net_dma_begin(r->nd, DMA_BYTES))
        return 0;
    for (i = 0; i < RX_COUNT; i++) {
        r->rx_buf[i] = (u8 *)net_dma_alloc(r->nd, RX_SIZE);
        r->rx_urb[i].Tag = i;
        r->rx_urb[i].Owner = r;
    }
    for (i = 0; i < TX_COUNT; i++) {
        r->tx_buf[i] = (u8 *)net_dma_alloc(r->nd, TX_SIZE);
        r->tx_urb[i].Tag = i;
        r->tx_urb[i].Owner = r;
        r->tx_urb[i].Busy = 0;
    }
    return r->rx_buf[RX_COUNT - 1] && r->tx_buf[TX_COUNT - 1];
}

static RNDIS *take_instance(NET_DEV *nd)
{
    u32 i;

    for (i = 0; i < RNDIS_MAX; i++) {
        RNDIS *r = &rndis_inst[i];

        if (r->in_use && r->nd != nd)
            continue;
        r->in_use = 1;
        r->nd = nd;
        r->dev = nd->usb;
        r->st = &rndis_stats[i];
        r->open_slot = NONE;
        r->intr_urb.Owner = r;
        nd->priv = r;
        return r;
    }
    return 0;
}

static u32 rndis_start(NET_DEV *nd)
{
    RNDIS *r = take_instance(nd);
    u32 i, status, actual, total, id;
    RNDIS_REPLY reply;
    u8 filter[4];

    if (!r) {
        nd->st->step = RNDIS_STEP_DMA;
        return RNDIS_STEP_DMA;
    }
    r->running = 0;
    r->rx_consecutive_errors = 0;
    r->intr_consecutive_errors = 0;
    r->ep_in = r->ep_out = r->ep_intr = 0;
    if (!alloc_buffers(r))
        return fail(r, RNDIS_STEP_DMA, 0);

    status = control(r, 0x80, 0x06, 0x0200, 0, 9, &actual);
    if (!NT_SUCCESS(status) || actual < 9)
        return fail(r, RNDIS_STEP_CONFIG_DESC, status);
    total = (u32)(r->ctrl_buf[2] | r->ctrl_buf[3] << 8);
    if (total > CTRL_SIZE)
        return fail(r, RNDIS_STEP_CONFIG_DESC, total);
    status = control(r, 0x80, 0x06, 0x0200, 0, (u16)total, &actual);
    if (!NT_SUCCESS(status) || !rndis_parse_config(r->ctrl_buf, actual, &r->layout))
        return fail(r, RNDIS_STEP_LAYOUT, status);

    status = control(r, 0x00, 0x09, r->layout.config_value, 0, 0, 0);
    if (!NT_SUCCESS(status))
        return fail(r, RNDIS_STEP_SET_CONFIG, status);
    if (r->layout.data_alt) {
        status = control(r, 0x01, 0x0B, r->layout.data_alt, r->layout.data_interface, 0, 0);
        if (!NT_SUCCESS(status))
            return fail(r, RNDIS_STEP_SET_INTERFACE, status);
    }

    id = ++r->request_id;
    status = transact(r, rndis_build_init(r->ctrl_buf, id, rndis_rx_max < RX_SIZE ?
                                       rndis_rx_max : RX_SIZE), id, &reply);
    r->st->init_status = status;
    if (!NT_SUCCESS(status))
        return fail(r, RNDIS_STEP_INIT, status);
    r->st->max_packets = reply.max_packets;
    r->st->max_transfer = reply.max_transfer;
    r->st->alignment = reply.alignment;
    r->tx_limit = reply.max_transfer ? reply.max_transfer : TX_SIZE;
    r->tx_max_packets = reply.max_packets ? reply.max_packets : 1;
    r->tx_align = 1u << (reply.alignment < 6 ? reply.alignment : 6);

    status = query(r, OID_802_3_PERMANENT_ADDRESS, &reply);
    if (!NT_SUCCESS(status) || reply.info_length < 6)
        return fail(r, RNDIS_STEP_MAC, status);
    for (i = 0; i < 6; i++)
        nd->mac[i] = reply.info[i];

    filter[0] = (u8)RNDIS_FILTER_DEFAULT;
    filter[1] = filter[2] = filter[3] = 0;
    id = ++r->request_id;
    status = transact(r, rndis_build_set(r->ctrl_buf, id, OID_GEN_CURRENT_PACKET_FILTER,
                                      filter, 4), id, &reply);
    r->st->filter_status = status;
    if (!NT_SUCCESS(status))
        return fail(r, RNDIS_STEP_FILTER, status);

    /* Optional: a device that does not answer these still carries frames. */
    if (NT_SUCCESS(query(r, OID_GEN_MEDIA_CONNECT_STATUS, &reply)) &&
        reply.info_length >= 4) {
        r->st->media_status = le32(reply.info);
        net_link_report(r->nd, r->st->media_status == 0);
    }
    if (NT_SUCCESS(query(r, OID_GEN_LINK_SPEED, &reply)) && reply.info_length >= 4) {
        r->st->link_speed = le32(reply.info);
        net_speed_report(r->nd, r->st->link_speed / 10000u);
    }

    r->ep_in = usb_open(r->dev, USB_TYPE_BULK, r->layout.ep_in, r->layout.in_max, 0);
    r->ep_out = usb_open(r->dev, USB_TYPE_BULK, r->layout.ep_out, r->layout.out_max, 0);
    if (r->layout.ep_intr)
        r->ep_intr = usb_open(r->dev, USB_TYPE_INTERRUPT, r->layout.ep_intr, r->layout.intr_max,
                           r->layout.intr_interval);
    if (!r->ep_in || !r->ep_out)
        return fail(r, RNDIS_STEP_ENDPOINTS, usb_stats.last_open_status);

    r->tx_next = 0;
    r->tx_inflight = 0;
    r->open_slot = NONE;
    r->running = 1;
    mem_sync();
    for (i = 0; i < RX_COUNT; i++)
        rx_queue(&r->rx_urb[i]);
    if (r->ep_intr)
        intr_queue(r);
    r->nd->st->step = RNDIS_STEP_OK;
    return 0;
}

static void rndis_detached(NET_DEV *nd)
{
    RNDIS *r = (RNDIS *)nd->priv;

    if (!r)
        return;
    r->in_use = 0;
    r->running = 0;
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
    .name = "rndis",
    .kind = XE_KIND_TETHER,
    .match = rndis_match,
    .start = rndis_start,
    .detached = rndis_detached,
    .xmit = rndis_xmit,
    .tx_space = rndis_tx_space,
};
