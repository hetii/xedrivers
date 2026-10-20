/* The CDC Ethernet driver: an Ethernet frame pipe over CDC ECM or CDC NCM. Written
 * for the RTL8156 (drivers/rtl8156 wraps it) and generic in what it parses: any
 * device whose configuration describes an ECM or NCM function will do.
 *
 * Bring-up, all from the worker thread at PASSIVE_LEVEL, each control transfer
 * waited for:
 *   GET_DESCRIPTOR device, then every configuration -- pick the one for the
 *   transport xe_config asks for (cdc.c), or the other if the device has only
 *   that (ECM is configuration 3 on the RTL8156, NCM 2)
 *   GET_DESCRIPTOR string iMACAddress       the card's own address
 *   SET_CONFIGURATION                       the card's firmware takes over MAC/PHY
 *   NCM only, with the data interface at alt 0 as the spec requires:
 *     GET_NTB_PARAMETERS, SET_NTB_INPUT_SIZE
 *   SET_INTERFACE data, alt 1               the alt setting with the bulk pair
 *   open bulk IN, bulk OUT, interrupt IN
 *   SET_ETHERNET_PACKET_FILTER              promiscuous: whoever consumes frames
 *                                           filters them by destination
 * then the receive ring stays queued for good, each request resubmitted from its
 * own completion, and one interrupt IN request carries link notifications.
 *
 * Two transports, and why ECM is the default. In ECM a transfer is a frame; in NCM
 * it is a transfer block (NTB) holding many. The hope was that fewer, larger
 * transfers would lift throughput. Measured on this console, they do not: both
 * reach the same ceilings, which are the console's USB, not the transfer count --
 *   - receive 242 Mbit/s (ECM) / 250 (NCM), with 32 or 64 receive requests alike,
 *     the ~31 MB/s the twin card also manages on the PC's own USB 2.0 port;
 *   - transmit 188 (ECM) / 193 (NCM) Mbit/s, 15.9k frames a second whether each
 *     frame is a transfer or ten share one (cdc_tx_eager 1: 12293 NTBs carried
 *     122911 frames) -- about 24 MB/s out;
 *   - FTP: ECM 19.6 MB/s down / 20.5 up; NCM 20.2 / 14.1 with 16 KB blocks (the
 *     card holds received frames back to fill a block, and TCP slows) and
 *     19.7 / 18.9 with 4 KB ones.
 * NCM transmit once sent nothing at all: wNdpOutPayloadRemainder applies to the
 * payload after the 14-byte Ethernet header (see place() in cdc.c), and frames
 * placed by it as their own offset are accepted and dropped by the RTL8156B.
 * Found by capturing Linux cdc_ncm with usbmon (tests/usbmon_dump.py).
 * So NCM works and stays selectable (TRANSPORT=ncm, NTB_SIZE), and ECM ships.
 *
 * ECM: a transfer is one frame, ended by a short packet; a frame of exactly n*512
 * bytes gets one byte of padding instead of a zero-length packet (as Linux usbnet).
 *
 * NCM transmit aggregates adaptively: with fewer than `cdc_tx_eager` blocks in
 * flight the open block goes out at once (latency); otherwise frames are added to
 * it and it goes when a block completes or it fills (throughput). Every submit is
 * made after the transmit lock is released: a transfer that completed
 * synchronously would otherwise re-enter tx_complete and deadlock on it.
 *
 * Measured: more than about 100 requests queued at once on this console hangs it
 * (RX 64 + TX 128 did, every time; 64 + 64 does not), so the defaults stay well
 * under that.
 */

#include "cdc_net.h"
#include "cdc.h"
#include "net.h"
#include "usb.h"
#include "config.h"
#include "prof.h"

/* Ring depths and the NTB size are build parameters (make RX_COUNT=... etc.). */
#ifndef RX_COUNT
#define RX_COUNT   32           /* ECM: one frame each */
#endif
#ifndef TX_COUNT
#define TX_COUNT   64
#endif
#ifndef NCM_RX_COUNT
#define NCM_RX_COUNT 16         /* NCM: one NTB each */
#endif
#ifndef NCM_TX_COUNT
#define NCM_TX_COUNT 16
#endif
#ifndef NTB_SIZE
#define NTB_SIZE   16384
#endif

#define FRAME_SIZE 2048         /* a multiple of 512, so a frame never babbles */
#define CTRL_SIZE  512
#define INTR_SIZE  64

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define RX_SLOTS   MAX(RX_COUNT, NCM_RX_COUNT)
#define TX_SLOTS   MAX(TX_COUNT, NCM_TX_COUNT)
#define RX_AREA    MAX(RX_COUNT * FRAME_SIZE, NCM_RX_COUNT * NTB_SIZE)
#define TX_AREA    MAX(TX_COUNT * FRAME_SIZE, NCM_TX_COUNT * NTB_SIZE)
#define DMA_BYTES  (RX_AREA + TX_AREA + 2 * 2048)  /* alignment slack */

#define RX_ERROR_LIMIT   64     /* consecutive failures before the ring stops */
#define INTR_ERROR_LIMIT 16
#define NONE             0xFFFFFFFFu

volatile u32 cdc_tx_eager = 2;
volatile u32 cdc_ncm_ndp_end = 1;
volatile CDC_STATS cdc_stats[CDC_MAX];

#define NOTE_KINDS 4

/* One device's worth of everything above: CDC_MAX of them, so two CDC cards (or a
 * card and an RTL8156) run at once. Completions find theirs through the request's
 * Owner, the receive DPC through its context. */
typedef struct CDC {
    u32 in_use;
    NET_DEV *nd;
    USB_DEV *dev;
    volatile CDC_STATS *st;
    CDC_LAYOUT layout;
    NCM_PARAMS ncm_params;
    u32 ncm;                    /* this bring-up uses NCM */
    void *ep_in;
    void *ep_out;
    void *ep_intr;
    URB rx_urb[RX_SLOTS];
    u8 *rx_buf[RX_SLOTS];
    u32 rx_count;
    u32 rx_size;
    u32 rx_consecutive_errors;
    URB tx_urb[TX_SLOTS];
    u8 *tx_buf[TX_SLOTS];
    u32 tx_len[TX_SLOTS];
    u32 tx_frames_in[TX_SLOTS];
    u32 tx_count;
    u32 tx_size;
    u32 tx_next;
    u32 tx_inflight;
    KSPIN_LOCK tx_lock;
    NCM_TX ntb;                 /* the block being filled */
    u32 ntb_slot;
    u16 ntb_sequence;
    URB intr_urb;
    u8 *intr_buf;
    u32 intr_consecutive_errors;
    u8 *ctrl_buf;
    volatile u32 running;
    KDPC rx_dpc;
    KSPIN_LOCK rx_lock;
    URB *rx_head, *rx_tail;
    u16 bcd_usb;                /* the device descriptor's, read in find_config */
    u8 last_note[NOTE_KINDS][16];
    u32 last_note_len[NOTE_KINDS];
    volatile u32 intr_parked;   /* 1: waiting for the tick to re-arm */
    u32 intr_parked_ticks;
} CDC;

static CDC cdc_inst[CDC_MAX];








static u16 le16(const u8 *p)
{
    return (u16)(p[0] | (p[1] << 8));
}

/* --- receive ----------------------------------------------------------------- */

static void rx_complete(URB *urb, u32 status);

static void rx_queue(URB *urb)
{
    CDC *c = (CDC *)urb->Owner;
    usb_prepare(urb, c->dev, c->ep_in, rx_complete, c->rx_buf[urb->Tag], c->rx_size, 0);
    usb_submit(urb);
}

static void deliver(const u8 *frame, u32 length, void *context)
{
    CDC *c = (CDC *)context;

    net_frame_in(c->nd, frame, length);
}

/* Where received frames are handed up. Inline, they are handed up from the USB
 * completion on the USB processor, and xnet's whole TCP receive runs there too,
 * serialised with the USB stack itself. With cdc_rx_cpu = N+1 they go to a DPC on
 * hardware thread N instead, and the request is resubmitted from there once its
 * frames are delivered, so USB completions and TCP run side by side. Set at run
 * time (xectl.py set cdc_rx_cpu 4); takes effect between batches. Gains nothing
 * measurable for FTP (19.5-20.5 MB/s up on every thread), and 1 under the TCP
 * server at full rate once stopped the console's network until a power cycle --
 * keep it 0. */
volatile u32 cdc_rx_cpu = 0;


static void rx_process(URB *urb)
{
    CDC *c = (CDC *)urb->Owner;
    if (c->ncm) {
        c->st->rx_ntbs++;
        if (cdc_ncm_parse_ntb(c->rx_buf[urb->Tag], urb->Actual, deliver, c) < 0)
            c->st->rx_ntb_errors++;
    } else {
        deliver(c->rx_buf[urb->Tag], urb->Actual, c);
    }
}

static void rx_dpc_routine(KDPC *dpc, void *context, void *a1, void *a2)
{
    CDC *c = (CDC *)context;
    URB *list, *next;

    (void)dpc;
    (void)a1;
    (void)a2;
    KeAcquireSpinLockAtRaisedIrql(&c->rx_lock);
    list = c->rx_head;
    c->rx_head = c->rx_tail = 0;
    KeReleaseSpinLockFromRaisedIrql(&c->rx_lock);

    for (; list; list = next) {
        next = list->Next;
        list->Next = 0;
        if (!c->running || !c->dev->active) {
            list->Busy = 0;
            continue;
        }
        rx_process(list);
        rx_queue(list);
    }
}

static void rx_complete_timed(URB *urb, u32 status)
{
    CDC *c = (CDC *)urb->Owner;
    u32 cpu;

    if (!c->running || !c->dev->active) {
        urb->Busy = 0;
        return;
    }

    if (NT_SUCCESS(status)) {
        c->rx_consecutive_errors = 0;
        cpu = cdc_rx_cpu;
        if (cpu) {
            c->st->rx_deferred++;
            KeAcquireSpinLockAtRaisedIrql(&c->rx_lock);
            urb->Next = 0;
            if (c->rx_tail)
                c->rx_tail->Next = urb;
            else
                c->rx_head = urb;
            c->rx_tail = urb;
            if (!c->rx_dpc.InsertedNumber)
                c->rx_dpc.TargetNumber = (u8)cpu;
            KeReleaseSpinLockFromRaisedIrql(&c->rx_lock);
            KeInsertQueueDpc(&c->rx_dpc, 0, 0);
            return;
        }
        rx_process(urb);
    } else {
        c->nd->st->rx_errors++;
        c->nd->st->rx_last_error = status;
        if (++c->rx_consecutive_errors > RX_ERROR_LIMIT) {
            c->nd->st->rx_stopped++;
            urb->Busy = 0;
            return;
        }
    }
    rx_queue(urb);
}

/* The count is read first: the request may be queued again before it returns. */
static void rx_complete(URB *urb, u32 status)
{
    u32 actual = urb->Actual;
    PROF_START(t);

    rx_complete_timed(urb, status);
    PROF_END(rx_done, t, actual);
    (void)actual;
}

/* --- transmit ---------------------------------------------------------------- */

static void tx_complete(URB *urb, u32 status);

static void tx_send(CDC *c, u32 i)
{
    if (i == NONE)
        return;
    usb_prepare(&c->tx_urb[i], c->dev, c->ep_out, tx_complete, c->tx_buf[i], c->tx_len[i], 0);
    usb_submit(&c->tx_urb[i]);
}

/* Close the open NTB and return its slot for sending, or NONE. Lock held. */
static u32 ntb_close_locked(CDC *c)
{
    u32 i = c->ntb_slot;

    if (i == NONE || !c->ntb.count)
        return NONE;
    c->tx_len[i] = cdc_ncm_tx_finish(&c->ntb, c->layout.out_max);
    c->tx_frames_in[i] = c->ntb.count;
    c->ntb_slot = NONE;
    c->tx_inflight++;
    c->st->tx_ntbs++;
    return i;
}

/* Take the next slot and start a block in it; 0 if the ring is full. Lock held. */
static u32 ntb_open_locked(CDC *c)
{
    u32 i = c->tx_next;

    if (c->tx_urb[i].Busy)
        return 0;
    c->tx_urb[i].Busy = 1;
    c->tx_next = (i + 1) % c->tx_count;
    c->ntb_slot = i;
    cdc_ncm_tx_begin(&c->ntb, c->tx_buf[i], c->tx_size, &c->ncm_params, c->ntb_sequence++,
                     cdc_ncm_ndp_end);
    return 1;
}

static void tx_complete(URB *urb, u32 status)
{
    CDC *c = (CDC *)urb->Owner;
    u32 irql, send = NONE, i = urb->Tag, length = urb->Length;
    PROF_START(t);

    irql = KfAcquireSpinLock(&c->tx_lock);
    if (NT_SUCCESS(status)) {
        c->nd->st->tx_frames += c->tx_frames_in[i];
        net_add64(&c->nd->st->tx_bytes, &c->nd->st->tx_bytes_hi, urb->Length);
    } else {
        c->nd->st->tx_errors++;
        c->nd->st->tx_last_error = status;
    }
    c->tx_inflight--;
    c->nd->st->tx_inflight = c->tx_inflight;
    urb->Busy = 0;
    if (c->ncm && c->tx_inflight < cdc_tx_eager)
        send = ntb_close_locked(c);
    KfReleaseSpinLock(&c->tx_lock, irql);

    tx_send(c, send);
    net_tx_done(c->nd);
    PROF_END(tx_done, t, length);
    (void)length;
}

u32 cdc_tx_space(NET_DEV *nd)
{
    CDC *c = (CDC *)nd->priv;

    if (!c->running)
        return 0;
    if (c->ncm && c->ntb_slot != NONE)
        return 1;
    return !c->tx_urb[c->tx_next].Busy;
}

static u32 tx_ecm(CDC *c, const u8 *frame, u32 length)
{
    u32 irql, i, n;
    u8 *dst;

    /* Slots are taken in order and complete in order on one endpoint, so a busy
     * next slot means the ring is full. */
    irql = KfAcquireSpinLock(&c->tx_lock);
    i = c->tx_next;
    if (c->tx_urb[i].Busy) {
        KfReleaseSpinLock(&c->tx_lock, irql);
        c->nd->st->tx_drops++;
        return 0;
    }
    c->tx_urb[i].Busy = 1;
    c->tx_next = (i + 1) % c->tx_count;
    c->tx_inflight++;
    c->nd->st->tx_inflight = c->tx_inflight;
    KfReleaseSpinLock(&c->tx_lock, irql);

    dst = c->tx_buf[i];
    {
        PROF_START(t);
        xe_copy(dst, frame, length);
        PROF_END(copy, t, length);
    }
    n = length;
    if (c->layout.out_max && (n % c->layout.out_max) == 0)
        dst[n++] = 0;
    c->tx_len[i] = n;
    c->tx_frames_in[i] = 1;
    tx_send(c, i);
    return 1;
}

static u32 tx_ncm(CDC *c, const u8 *frame, u32 length)
{
    u32 irql, full = NONE, now = NONE, queued = 0;

    irql = KfAcquireSpinLock(&c->tx_lock);
    if (c->ntb_slot == NONE && !ntb_open_locked(c))
        goto out;
    if (!cdc_ncm_tx_add(&c->ntb, frame, length)) {
        full = ntb_close_locked(c);
        if (!ntb_open_locked(c) || !cdc_ncm_tx_add(&c->ntb, frame, length))
            goto out;
    }
    queued = 1;
    if (c->tx_inflight < cdc_tx_eager)
        now = ntb_close_locked(c);
out:
    c->nd->st->tx_inflight = c->tx_inflight;
    KfReleaseSpinLock(&c->tx_lock, irql);

    tx_send(c, full);
    tx_send(c, now);
    if (!queued)
        c->nd->st->tx_drops++;
    return queued;
}

u32 cdc_xmit(NET_DEV *nd, const u8 *frame, u32 length)
{
    CDC *c = (CDC *)nd->priv;

    if (!c->running) {
        c->nd->st->tx_drops++;
        return 0;
    }
    return c->ncm ? tx_ncm(c, frame, length) : tx_ecm(c, frame, length);
}

/* --- link notifications ------------------------------------------------------ */

static void intr_complete(URB *urb, u32 status);

static void intr_queue(CDC *c)
{
    usb_prepare(&c->intr_urb, c->dev, c->ep_intr, intr_complete, c->intr_buf, c->layout.intr_max, 0);
    usb_submit(&c->intr_urb);
}

/* The interrupt endpoint, and a card that never stops talking on it.
 *
 * A notification endpoint is meant to answer a poll only when it has news, and NAK
 * otherwise. The RTL8156 in ECM answers every poll with the same
 * CONNECTION_SPEED_CHANGE and NETWORK_CONNECTION -- under Linux cdc_ether too,
 * about four a second at the 128 ms its bInterval asks for. Two things kept that
 * from becoming a stream here:
 *   - UsbdOpenEndpoint's interval is in milliseconds, polled at no longer than
 *     32 ms (measured: 11 gave 62.5 completions a second, 16 to 255 gave 31.2),
 *     while a high-speed bInterval is an exponent of 125 us microframes -- passed
 *     raw, 11 polled every 16 ms instead of every 128. intr_interval_ms converts.
 *   - a notification identical to the last one of its kind (bNotification) is not
 *     followed by a new request at once -- the RTL8156 alternates two kinds, so it
 *     is the last of each that counts: the endpoint is parked and re-armed from the worker's tick about a
 *     second later. A change -- link down, another speed -- is answered at once
 *     and re-armed at once. So a cable pulled out is seen within about a second;
 *     a device that behaves never parks.
 * Neither moved throughput (TCP 21.9 MB/s out at 62.5, 31.2 and no notifications
 * a second); it is the bus and the USB stack that are spared ~60 pointless
 * transfers a second. */
#define INTR_QUIET_TICKS 30     /* ~1 s of worker ticks, ~33 ms each as measured */


static u32 intr_interval_ms(CDC *c, u32 b_interval)
{
    u32 ms = b_interval;

    if (c->bcd_usb >= 0x0200) {    /* high speed: 2^(bInterval-1) microframes */
        if (b_interval < 1)
            b_interval = 1;
        ms = b_interval > 9 ? 256 : (1u << (b_interval - 1)) / 8;
    }
    if (ms < 1)
        ms = 1;
    return ms > 32 ? 32 : ms;
}

/* 1 if `data` repeats the last notification of its kind; otherwise it is
 * remembered (in a free slot, or the last one) and 0. */
static u32 repeat_of_last(CDC *c, const u8 *data, u32 length)
{
    u32 k, i, free = NOTE_KINDS - 1;

    if (length < 2 || length > sizeof(c->last_note[0]))
        return 0;
    for (k = 0; k < NOTE_KINDS; k++) {
        if (!c->last_note_len[k]) {
            free = k;
            break;
        }
        if (c->last_note[k][1] != data[1])
            continue;
        if (c->last_note_len[k] == length) {
            for (i = 0; i < length && c->last_note[k][i] == data[i]; i++)
                ;
            if (i == length)
                return 1;
        }
        free = k;
        break;
    }
    for (i = 0; i < length; i++)
        c->last_note[free][i] = data[i];
    c->last_note_len[free] = length;
    return 0;
}

static void intr_complete(URB *urb, u32 status)
{
    CDC *c = (CDC *)urb->Owner;
    u32 at, used;
    CDC_NOTE note;

    if (!c->running || !c->dev->active) {
        urb->Busy = 0;
        return;
    }

    if (NT_SUCCESS(status)) {
        c->intr_consecutive_errors = 0;
        for (at = 0; at < urb->Actual; at += used) {
            used = cdc_parse_notification(c->intr_buf + at, urb->Actual - at, &note);
            if (!used)
                break;
            c->st->intr_events++;
            c->st->last_notify[0] = (u32)c->intr_buf[at + 1] << 16 |
                                       le16(c->intr_buf + at + 2);
            if (note.kind == CDC_NOTE_LINK) {
                net_link_report(c->nd, note.link_up);
            } else if (note.kind == CDC_NOTE_SPEED) {
                c->st->last_notify[1] = note.down_bps;
                net_speed_report(c->nd, note.down_bps / 1000000u);
            }
        }
    } else {
        c->st->intr_errors++;
        if (++c->intr_consecutive_errors > INTR_ERROR_LIMIT) {
            urb->Busy = 0;
            return;
        }
    }
    if (NT_SUCCESS(status) && urb->Actual && repeat_of_last(c, c->intr_buf, urb->Actual)) {
        c->st->intr_parked++;
        urb->Busy = 0;
        c->intr_parked_ticks = 0;
        mem_sync();
        c->intr_parked = 1;
        return;
    }
    intr_queue(c);
}

/* From the worker: re-arm a parked interrupt endpoint once it has been quiet. */
void cdc_tick(NET_DEV *nd)
{
    CDC *c = (CDC *)nd->priv;

    if (!c->intr_parked || !c->running || !c->dev->active)
        return;
    if (++c->intr_parked_ticks < INTR_QUIET_TICKS)
        return;
    c->intr_parked = 0;
    intr_queue(c);
}

/* --- bring-up ---------------------------------------------------------------- */

static u32 cdc_control(CDC *c, u8 type, u8 request, u16 value, u16 index, u16 length, u32 *actual)
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
    return usb_control(c->dev, setup, c->ctrl_buf, length, actual, 1000);
}

static u8 *cdc_ctrl_buffer(CDC *c)
{
    return c->ctrl_buf;
}

static u32 fail(CDC *c, u32 step, u32 status)
{
    c->nd->st->step = step;
    c->nd->st->step_status = status;
    return step;
}

/* The control buffers once, from the core's scratch block. */
static u32 alloc_control(CDC *c)
{
    if (!c->ctrl_buf) {
        c->ctrl_buf = (u8 *)net_dma_scratch(CTRL_SIZE);
        c->intr_buf = (u8 *)net_dma_scratch(INTR_SIZE);
    }
    return c->ctrl_buf && c->intr_buf;
}

/* The rings are carved again on every bring-up, because ECM and NCM want different
 * counts and sizes out of the same block. */
static u32 alloc_rings(CDC *c)
{
    u32 i;

    if (!net_dma_begin(c->nd, DMA_BYTES))
        return 0;

    c->rx_count = c->ncm ? NCM_RX_COUNT : RX_COUNT;
    c->rx_size = c->ncm ? NTB_SIZE : FRAME_SIZE;
    c->tx_count = c->ncm ? NCM_TX_COUNT : TX_COUNT;
    c->tx_size = c->ncm ? NTB_SIZE : FRAME_SIZE;

    for (i = 0; i < c->rx_count; i++) {
        c->rx_buf[i] = (u8 *)net_dma_alloc(c->nd, c->rx_size);
        c->rx_urb[i].Tag = i;
        c->rx_urb[i].Owner = c;
    }
    for (i = 0; i < c->tx_count; i++) {
        c->tx_buf[i] = (u8 *)net_dma_alloc(c->nd, c->tx_size);
        c->tx_urb[i].Tag = i;
        c->tx_urb[i].Owner = c;
    }
    return c->rx_buf[c->rx_count - 1] && c->tx_buf[c->tx_count - 1];
}

/* Every configuration the device has, until one parses as the subclass wanted. */
static u32 find_config(CDC *c, u32 subclass)
{
    u32 index, actual, status, total, configs;

    status = cdc_control(c, 0x80, 0x06, 0x0100, 0, 18, &actual);
    if (!NT_SUCCESS(status) || actual < 18)
        return fail(c, CDC_STEP_DEVICE_DESC, status);
    c->bcd_usb = le16(c->ctrl_buf + 2);
    configs = c->ctrl_buf[17];

    for (index = 0; index < configs; index++) {
        status = cdc_control(c, 0x80, 0x06, (u16)(0x0200 | index), 0, 9, &actual);
        if (!NT_SUCCESS(status) || actual < 9)
            return fail(c, CDC_STEP_CONFIG_DESC, status);
        total = le16(c->ctrl_buf + 2);
        if (total > CTRL_SIZE)
            continue;
        status = cdc_control(c, 0x80, 0x06, (u16)(0x0200 | index), 0, (u16)total, &actual);
        if (!NT_SUCCESS(status))
            return fail(c, CDC_STEP_CONFIG_DESC, status);
        if (cdc_parse_config(c->ctrl_buf, actual, &c->layout) && c->layout.subclass == subclass)
            return 0;
    }
    return fail(c, CDC_STEP_LAYOUT, configs);
}

/* GET_NTB_PARAMETERS, then ask for input blocks no larger than our buffers. The
 * second is optional in the spec, so its failure is recorded and lived with --
 * unless the device's own size is larger than a buffer, which would babble. */
static u32 ncm_setup(CDC *c)
{
    u32 status, actual, want = NTB_SIZE;

    status = cdc_control(c, 0xA1, 0x80, 0, c->layout.comm_interface, 28, &actual);
    if (!NT_SUCCESS(status) || !cdc_ncm_parse_params(c->ctrl_buf, actual, &c->ncm_params))
        return fail(c, CDC_STEP_NTB_PARAMS, status);
    c->st->ntb_in_max = c->ncm_params.in_max;
    c->st->ntb_out_max = c->ncm_params.out_max;
    c->st->ntb_out_align = (u32)c->ncm_params.out_divisor << 16 |
                              c->ncm_params.out_remainder;
    c->st->ntb_out_datagrams = c->ncm_params.out_max_datagrams;

    c->st->ntb_formats = c->ncm_params.formats;

    /* What Linux cdc_ncm sends next, all while the data interface is at alt 0,
     * all optional in the spec and so recorded rather than required: NTB16 when
     * the device also does NTB32, no CRC appended, and the largest datagram
     * from the Ethernet functional descriptor. A device left expecting CRCs or
     * NTB32 accepts our NTB16/NCM0 blocks and transmits nothing. */
    c->st->ntb_format_status = 0xFFFFFFFFu;
    if (c->ncm_params.formats & 2)
        c->st->ntb_format_status = cdc_control(c, 0x21, 0x84, 0, c->layout.comm_interface,
                                              0, 0);
    c->st->crc_status = cdc_control(c, 0x21, 0x8A, 0, c->layout.comm_interface, 0, 0);
    if (c->layout.max_segment) {
        c->ctrl_buf[0] = (u8)c->layout.max_segment;
        c->ctrl_buf[1] = (u8)(c->layout.max_segment >> 8);
        c->st->dgram_status = cdc_control(c, 0x21, 0x88, 0, c->layout.comm_interface, 2, 0);
    }

    if (c->ncm_params.in_max != want) {
        c->ctrl_buf[0] = (u8)want;
        c->ctrl_buf[1] = (u8)(want >> 8);
        c->ctrl_buf[2] = (u8)(want >> 16);
        c->ctrl_buf[3] = (u8)(want >> 24);
        status = cdc_control(c, 0x21, 0x86, 0, c->layout.comm_interface, 4, 0);
        c->st->ntb_input_status = status;
        if (!NT_SUCCESS(status) && c->ncm_params.in_max > NTB_SIZE)
            return fail(c, CDC_STEP_NTB_PARAMS, status);
    }
    return 0;
}

/* A free instance for `nd`'s device, or 0 when CDC_MAX are in use. */
static CDC *take_instance(NET_DEV *nd)
{
    u32 i;

    for (i = 0; i < CDC_MAX; i++) {
        CDC *c = &cdc_inst[i];

        if (c->in_use && c->nd != nd)
            continue;
        c->in_use = 1;
        c->nd = nd;
        c->dev = nd->usb;
        c->st = &cdc_stats[i];
        c->ntb_slot = NONE;
        c->intr_urb.Owner = c;
        nd->priv = c;
        return c;
    }
    return 0;
}

u32 cdc_start(NET_DEV *nd)
{
    CDC *c = take_instance(nd);
    u32 i, status, actual;

    if (!c) {
        nd->st->step = CDC_STEP_DMA;
        return CDC_STEP_DMA;
    }
    c->running = 0;
    c->intr_parked = 0;
    for (i = 0; i < NOTE_KINDS; i++)
        c->last_note_len[i] = 0;
    c->rx_consecutive_errors = 0;
    c->intr_consecutive_errors = 0;
    c->ep_in = c->ep_out = c->ep_intr = 0;
    c->ncm = xe_config.transport == XE_TRANSPORT_NCM;

    if (!alloc_control(c))
        return fail(c, CDC_STEP_DMA, 0);

    /* The transport asked for, else the other one: a device may have only one. */
    if (find_config(c, c->ncm ? CDC_SUBCLASS_NCM : CDC_SUBCLASS_ECM)) {
        if (c->nd->st->step != CDC_STEP_LAYOUT)
            return c->nd->st->step;
        c->ncm = !c->ncm;
        if (find_config(c, c->ncm ? CDC_SUBCLASS_NCM : CDC_SUBCLASS_ECM))
            return c->nd->st->step;
    }
    if (!alloc_rings(c))
        return fail(c, CDC_STEP_DMA, 0);
    c->st->transport = c->ncm;
    c->st->config_value = c->layout.config_value;
    c->st->data_interface = c->layout.data_interface;

    if (c->layout.mac_string) {
        status = cdc_control(c, 0x80, 0x06, (u16)(0x0300 | c->layout.mac_string), 0x0409, 255,
                         &actual);
        if (!NT_SUCCESS(status) || !cdc_parse_mac(c->ctrl_buf, actual, c->nd->mac))
            return fail(c, CDC_STEP_MAC, status);
    }

    status = cdc_control(c, 0x00, 0x09, c->layout.config_value, 0, 0, 0);
    if (!NT_SUCCESS(status))
        return fail(c, CDC_STEP_SET_CONFIG, status);

    if (c->ncm) {
        status = cdc_control(c, 0x01, 0x0B, 0, c->layout.data_interface, 0, 0);
        if (!NT_SUCCESS(status))
            return fail(c, CDC_STEP_SET_INTERFACE, status);
        if (ncm_setup(c))
            return c->nd->st->step;
    }

    status = cdc_control(c, 0x01, 0x0B, c->layout.data_alt, c->layout.data_interface, 0, 0);
    if (!NT_SUCCESS(status))
        return fail(c, CDC_STEP_SET_INTERFACE, status);

    c->ep_in = usb_open(c->dev, USB_TYPE_BULK, c->layout.ep_in, c->layout.in_max, 0);
    c->ep_out = usb_open(c->dev, USB_TYPE_BULK, c->layout.ep_out, c->layout.out_max, 0);
    if (c->layout.ep_intr)
        c->ep_intr = usb_open(c->dev, USB_TYPE_INTERRUPT, c->layout.ep_intr, c->layout.intr_max,
                           intr_interval_ms(c, c->layout.intr_interval));
    c->st->ep_in = (u32)(unsigned long)c->ep_in;
    c->st->ep_out = (u32)(unsigned long)c->ep_out;
    c->st->ep_intr = (u32)(unsigned long)c->ep_intr;
    if (!c->ep_in || !c->ep_out)
        return fail(c, CDC_STEP_ENDPOINTS, usb_stats.last_open_status);

    /* PROMISCUOUS | ALL_MULTICAST | DIRECTED | BROADCAST */
    status = cdc_control(c, 0x21, 0x43, 0x000F, c->layout.comm_interface, 0, 0);
    if (!NT_SUCCESS(status))
        return fail(c, CDC_STEP_FILTER, status);

    KeInitializeDpc(&c->rx_dpc, rx_dpc_routine, c);
    c->rx_head = c->rx_tail = 0;

    c->tx_next = 0;
    c->tx_inflight = 0;
    c->ntb_slot = NONE;
    for (i = 0; i < c->tx_count; i++)
        c->tx_urb[i].Busy = 0;
    c->nd->st->tx_inflight = 0;

    c->running = 1;
    mem_sync();
    for (i = 0; i < c->rx_count; i++)
        rx_queue(&c->rx_urb[i]);
    if (c->ep_intr)
        intr_queue(c);

    c->nd->st->step = CDC_STEP_OK;
    return 0;
}

/* From RemoveDevice, at DISPATCH_LEVEL: stop resubmitting; the close chain that
 * follows completes whatever is still queued. */
void cdc_detached(NET_DEV *nd)
{
    CDC *c = (CDC *)nd->priv;

    if (!c)
        return;
    c->in_use = 0;
    c->running = 0;
    mem_sync();
}

/* --- the generic driver ------------------------------------------------------- */

/* Any device that shows a CDC ECM or NCM communication interface in its current
 * configuration. Devices whose CDC function is in another configuration (the
 * RTL8156's is: its first one is Realtek's own) need a driver of their own that
 * knows to look, as drivers/rtl8156 does. */
static u32 cdc_match(const u8 *device, const u8 *interface)
{
    (void)device;
    if (interface && interface[5] == 0x02 &&
        (interface[6] == CDC_SUBCLASS_ECM || interface[6] == CDC_SUBCLASS_NCM))
        return XE_MATCH_CLASS;
    return XE_MATCH_NONE;
}

const XE_DRIVER xe_driver_cdc = {
    .name = "cdc",
    .kind = XE_KIND_WIRED,
    .match = cdc_match,
    .start = cdc_start,
    .detached = cdc_detached,
    .xmit = cdc_xmit,
    .tx_space = cdc_tx_space,
    .tick = cdc_tick,
};
