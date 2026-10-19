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
volatile CDC_STATS cdc_stats;

static CDC_LAYOUT layout;
static NCM_PARAMS ncm_params;
static u32 ncm;                 /* this bring-up uses NCM */
static void *ep_in;
static void *ep_out;
static void *ep_intr;

static URB rx_urb[RX_SLOTS];
static u8 *rx_buf[RX_SLOTS];
static u32 rx_count;
static u32 rx_size;
static u32 rx_consecutive_errors;

static URB tx_urb[TX_SLOTS];
static u8 *tx_buf[TX_SLOTS];
static u32 tx_len[TX_SLOTS];
static u32 tx_frames_in[TX_SLOTS];
static u32 tx_count;
static u32 tx_size;
static u32 tx_next;
static u32 tx_inflight;
static KSPIN_LOCK tx_lock;

static NCM_TX ntb;              /* the block being filled */
static u32 ntb_slot = NONE;
static u16 ntb_sequence;

static URB intr_urb;
static u8 *intr_buf;
static u32 intr_consecutive_errors;

static u8 *ctrl_buf;

static volatile u32 running;

static u16 le16(const u8 *p)
{
    return (u16)(p[0] | (p[1] << 8));
}

/* --- receive ----------------------------------------------------------------- */

static void rx_complete(URB *urb, u32 status);

static void rx_queue(URB *urb)
{
    usb_prepare(urb, ep_in, rx_complete, rx_buf[urb->Tag], rx_size, 0);
    usb_submit(urb);
}

static void deliver(const u8 *frame, u32 length, void *context)
{
    (void)context;
    net_frame_in(frame, length);
}

/* Where received frames are handed up. Inline, they are handed up from the USB
 * completion on the USB processor, and xnet's whole TCP receive runs there too,
 * serialised with the USB stack itself. With cdc_rx_cpu = N+1 they go to a DPC on
 * hardware thread N instead, and the request is resubmitted from there once its
 * frames are delivered, so USB completions and TCP run side by side. Set at run
 * time (xectl.py set cdc_rx_cpu 4); takes effect between batches. */
volatile u32 cdc_rx_cpu = 0;

static KDPC rx_dpc;
static KSPIN_LOCK rx_lock;
static URB *rx_head, *rx_tail;

static void rx_process(URB *urb)
{
    if (ncm) {
        cdc_stats.rx_ntbs++;
        if (cdc_ncm_parse_ntb(rx_buf[urb->Tag], urb->Actual, deliver, 0) < 0)
            cdc_stats.rx_ntb_errors++;
    } else {
        deliver(rx_buf[urb->Tag], urb->Actual, 0);
    }
}

static void rx_dpc_routine(KDPC *dpc, void *context, void *a1, void *a2)
{
    URB *list, *next;

    (void)dpc;
    (void)context;
    (void)a1;
    (void)a2;
    KeAcquireSpinLockAtRaisedIrql(&rx_lock);
    list = rx_head;
    rx_head = rx_tail = 0;
    KeReleaseSpinLockFromRaisedIrql(&rx_lock);

    for (; list; list = next) {
        next = list->Next;
        list->Next = 0;
        if (!running || !usb.active) {
            list->Busy = 0;
            continue;
        }
        rx_process(list);
        rx_queue(list);
    }
}

static void rx_complete_timed(URB *urb, u32 status)
{
    u32 cpu;

    if (!running || !usb.active) {
        urb->Busy = 0;
        return;
    }

    if (NT_SUCCESS(status)) {
        rx_consecutive_errors = 0;
        cpu = cdc_rx_cpu;
        if (cpu) {
            cdc_stats.rx_deferred++;
            KeAcquireSpinLockAtRaisedIrql(&rx_lock);
            urb->Next = 0;
            if (rx_tail)
                rx_tail->Next = urb;
            else
                rx_head = urb;
            rx_tail = urb;
            if (!rx_dpc.InsertedNumber)
                rx_dpc.TargetNumber = (u8)cpu;
            KeReleaseSpinLockFromRaisedIrql(&rx_lock);
            KeInsertQueueDpc(&rx_dpc, 0, 0);
            return;
        }
        rx_process(urb);
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

static void tx_send(u32 i)
{
    if (i == NONE)
        return;
    usb_prepare(&tx_urb[i], ep_out, tx_complete, tx_buf[i], tx_len[i], 0);
    usb_submit(&tx_urb[i]);
}

/* Close the open NTB and return its slot for sending, or NONE. Lock held. */
static u32 ntb_close_locked(void)
{
    u32 i = ntb_slot;

    if (i == NONE || !ntb.count)
        return NONE;
    tx_len[i] = cdc_ncm_tx_finish(&ntb, layout.out_max);
    tx_frames_in[i] = ntb.count;
    ntb_slot = NONE;
    tx_inflight++;
    cdc_stats.tx_ntbs++;
    return i;
}

/* Take the next slot and start a block in it; 0 if the ring is full. Lock held. */
static u32 ntb_open_locked(void)
{
    u32 i = tx_next;

    if (tx_urb[i].Busy)
        return 0;
    tx_urb[i].Busy = 1;
    tx_next = (i + 1) % tx_count;
    ntb_slot = i;
    cdc_ncm_tx_begin(&ntb, tx_buf[i], tx_size, &ncm_params, ntb_sequence++,
                     cdc_ncm_ndp_end);
    return 1;
}

static void tx_complete(URB *urb, u32 status)
{
    u32 irql, send = NONE, i = urb->Tag, length = urb->Length;
    PROF_START(t);

    irql = KfAcquireSpinLock(&tx_lock);
    if (NT_SUCCESS(status)) {
        net_stats.tx_frames += tx_frames_in[i];
        net_add64(&net_stats.tx_bytes, &net_stats.tx_bytes_hi, urb->Length);
    } else {
        net_stats.tx_errors++;
        net_stats.tx_last_error = status;
    }
    tx_inflight--;
    net_stats.tx_inflight = tx_inflight;
    urb->Busy = 0;
    if (ncm && tx_inflight < cdc_tx_eager)
        send = ntb_close_locked();
    KfReleaseSpinLock(&tx_lock, irql);

    tx_send(send);
    net_tx_done();
    PROF_END(tx_done, t, length);
    (void)length;
}

u32 cdc_tx_space(void)
{
    if (!running)
        return 0;
    if (ncm && ntb_slot != NONE)
        return 1;
    return !tx_urb[tx_next].Busy;
}

static u32 tx_ecm(const u8 *frame, u32 length)
{
    u32 irql, i, n;
    u8 *dst;

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
    tx_next = (i + 1) % tx_count;
    tx_inflight++;
    net_stats.tx_inflight = tx_inflight;
    KfReleaseSpinLock(&tx_lock, irql);

    dst = tx_buf[i];
    {
        PROF_START(t);
        xe_copy(dst, frame, length);
        PROF_END(copy, t, length);
    }
    n = length;
    if (layout.out_max && (n % layout.out_max) == 0)
        dst[n++] = 0;
    tx_len[i] = n;
    tx_frames_in[i] = 1;
    tx_send(i);
    return 1;
}

static u32 tx_ncm(const u8 *frame, u32 length)
{
    u32 irql, full = NONE, now = NONE, queued = 0;

    irql = KfAcquireSpinLock(&tx_lock);
    if (ntb_slot == NONE && !ntb_open_locked())
        goto out;
    if (!cdc_ncm_tx_add(&ntb, frame, length)) {
        full = ntb_close_locked();
        if (!ntb_open_locked() || !cdc_ncm_tx_add(&ntb, frame, length))
            goto out;
    }
    queued = 1;
    if (tx_inflight < cdc_tx_eager)
        now = ntb_close_locked();
out:
    net_stats.tx_inflight = tx_inflight;
    KfReleaseSpinLock(&tx_lock, irql);

    tx_send(full);
    tx_send(now);
    if (!queued)
        net_stats.tx_drops++;
    return queued;
}

u32 cdc_xmit(const u8 *frame, u32 length)
{
    if (!running) {
        net_stats.tx_drops++;
        return 0;
    }
    return ncm ? tx_ncm(frame, length) : tx_ecm(frame, length);
}

/* --- link notifications ------------------------------------------------------ */

static void intr_complete(URB *urb, u32 status);

static void intr_queue(void)
{
    usb_prepare(&intr_urb, ep_intr, intr_complete, intr_buf, layout.intr_max, 0);
    usb_submit(&intr_urb);
}

static void intr_complete(URB *urb, u32 status)
{
    u32 at, used;
    CDC_NOTE note;

    if (!running || !usb.active) {
        urb->Busy = 0;
        return;
    }

    if (NT_SUCCESS(status)) {
        intr_consecutive_errors = 0;
        for (at = 0; at < urb->Actual; at += used) {
            used = cdc_parse_notification(intr_buf + at, urb->Actual - at, &note);
            if (!used)
                break;
            cdc_stats.intr_events++;
            cdc_stats.last_notify[0] = (u32)intr_buf[at + 1] << 16 |
                                       le16(intr_buf + at + 2);
            if (note.kind == CDC_NOTE_LINK) {
                net_link_report(note.link_up);
            } else if (note.kind == CDC_NOTE_SPEED) {
                cdc_stats.last_notify[1] = note.down_bps;
                net_speed_report(note.down_bps / 1000000u);
            }
        }
    } else {
        cdc_stats.intr_errors++;
        if (++intr_consecutive_errors > INTR_ERROR_LIMIT) {
            urb->Busy = 0;
            return;
        }
    }
    intr_queue();
}

/* --- bring-up ---------------------------------------------------------------- */

u32 cdc_control(u8 type, u8 request, u16 value, u16 index, u16 length, u32 *actual)
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

u8 *cdc_ctrl_buffer(void)
{
    return ctrl_buf;
}

static u32 fail(u32 step, u32 status)
{
    net_stats.step = step;
    net_stats.step_status = status;
    return step;
}

/* The control buffers once, from the core's scratch block. */
static u32 alloc_control(void)
{
    if (!ctrl_buf) {
        ctrl_buf = (u8 *)net_dma_scratch(CTRL_SIZE);
        intr_buf = (u8 *)net_dma_scratch(INTR_SIZE);
    }
    return ctrl_buf && intr_buf;
}

/* The rings are carved again on every bring-up, because ECM and NCM want different
 * counts and sizes out of the same block. */
static u32 alloc_rings(void)
{
    u32 i;

    if (!net_dma_begin(DMA_BYTES))
        return 0;

    rx_count = ncm ? NCM_RX_COUNT : RX_COUNT;
    rx_size = ncm ? NTB_SIZE : FRAME_SIZE;
    tx_count = ncm ? NCM_TX_COUNT : TX_COUNT;
    tx_size = ncm ? NTB_SIZE : FRAME_SIZE;

    for (i = 0; i < rx_count; i++) {
        rx_buf[i] = (u8 *)net_dma_alloc(rx_size);
        rx_urb[i].Tag = i;
    }
    for (i = 0; i < tx_count; i++) {
        tx_buf[i] = (u8 *)net_dma_alloc(tx_size);
        tx_urb[i].Tag = i;
    }
    return rx_buf[rx_count - 1] && tx_buf[tx_count - 1];
}

/* Every configuration the device has, until one parses as the subclass wanted. */
static u32 find_config(u32 subclass)
{
    u32 index, actual, status, total, configs;

    status = cdc_control(0x80, 0x06, 0x0100, 0, 18, &actual);
    if (!NT_SUCCESS(status) || actual < 18)
        return fail(CDC_STEP_DEVICE_DESC, status);
    configs = ctrl_buf[17];

    for (index = 0; index < configs; index++) {
        status = cdc_control(0x80, 0x06, (u16)(0x0200 | index), 0, 9, &actual);
        if (!NT_SUCCESS(status) || actual < 9)
            return fail(CDC_STEP_CONFIG_DESC, status);
        total = le16(ctrl_buf + 2);
        if (total > CTRL_SIZE)
            continue;
        status = cdc_control(0x80, 0x06, (u16)(0x0200 | index), 0, (u16)total, &actual);
        if (!NT_SUCCESS(status))
            return fail(CDC_STEP_CONFIG_DESC, status);
        if (cdc_parse_config(ctrl_buf, actual, &layout) && layout.subclass == subclass)
            return 0;
    }
    return fail(CDC_STEP_LAYOUT, configs);
}

/* GET_NTB_PARAMETERS, then ask for input blocks no larger than our buffers. The
 * second is optional in the spec, so its failure is recorded and lived with --
 * unless the device's own size is larger than a buffer, which would babble. */
static u32 ncm_setup(void)
{
    u32 status, actual, want = NTB_SIZE;

    status = cdc_control(0xA1, 0x80, 0, layout.comm_interface, 28, &actual);
    if (!NT_SUCCESS(status) || !cdc_ncm_parse_params(ctrl_buf, actual, &ncm_params))
        return fail(CDC_STEP_NTB_PARAMS, status);
    cdc_stats.ntb_in_max = ncm_params.in_max;
    cdc_stats.ntb_out_max = ncm_params.out_max;
    cdc_stats.ntb_out_align = (u32)ncm_params.out_divisor << 16 |
                              ncm_params.out_remainder;
    cdc_stats.ntb_out_datagrams = ncm_params.out_max_datagrams;

    cdc_stats.ntb_formats = ncm_params.formats;

    /* What Linux cdc_ncm sends next, all while the data interface is at alt 0,
     * all optional in the spec and so recorded rather than required: NTB16 when
     * the device also does NTB32, no CRC appended, and the largest datagram
     * from the Ethernet functional descriptor. A device left expecting CRCs or
     * NTB32 accepts our NTB16/NCM0 blocks and transmits nothing. */
    cdc_stats.ntb_format_status = 0xFFFFFFFFu;
    if (ncm_params.formats & 2)
        cdc_stats.ntb_format_status = cdc_control(0x21, 0x84, 0, layout.comm_interface,
                                              0, 0);
    cdc_stats.crc_status = cdc_control(0x21, 0x8A, 0, layout.comm_interface, 0, 0);
    if (layout.max_segment) {
        ctrl_buf[0] = (u8)layout.max_segment;
        ctrl_buf[1] = (u8)(layout.max_segment >> 8);
        cdc_stats.dgram_status = cdc_control(0x21, 0x88, 0, layout.comm_interface, 2, 0);
    }

    if (ncm_params.in_max != want) {
        ctrl_buf[0] = (u8)want;
        ctrl_buf[1] = (u8)(want >> 8);
        ctrl_buf[2] = (u8)(want >> 16);
        ctrl_buf[3] = (u8)(want >> 24);
        status = cdc_control(0x21, 0x86, 0, layout.comm_interface, 4, 0);
        cdc_stats.ntb_input_status = status;
        if (!NT_SUCCESS(status) && ncm_params.in_max > NTB_SIZE)
            return fail(CDC_STEP_NTB_PARAMS, status);
    }
    return 0;
}

u32 cdc_start(void)
{
    u32 i, status, actual;

    running = 0;
    rx_consecutive_errors = 0;
    intr_consecutive_errors = 0;
    ep_in = ep_out = ep_intr = 0;
    ncm = xe_config.transport == XE_TRANSPORT_NCM;

    if (!alloc_control())
        return fail(CDC_STEP_DMA, 0);

    /* The transport asked for, else the other one: a device may have only one. */
    if (find_config(ncm ? CDC_SUBCLASS_NCM : CDC_SUBCLASS_ECM)) {
        if (net_stats.step != CDC_STEP_LAYOUT)
            return net_stats.step;
        ncm = !ncm;
        if (find_config(ncm ? CDC_SUBCLASS_NCM : CDC_SUBCLASS_ECM))
            return net_stats.step;
    }
    if (!alloc_rings())
        return fail(CDC_STEP_DMA, 0);
    cdc_stats.transport = ncm;
    cdc_stats.config_value = layout.config_value;
    cdc_stats.data_interface = layout.data_interface;

    if (layout.mac_string) {
        status = cdc_control(0x80, 0x06, (u16)(0x0300 | layout.mac_string), 0x0409, 255,
                         &actual);
        if (!NT_SUCCESS(status) || !cdc_parse_mac(ctrl_buf, actual, net_mac))
            return fail(CDC_STEP_MAC, status);
    }

    status = cdc_control(0x00, 0x09, layout.config_value, 0, 0, 0);
    if (!NT_SUCCESS(status))
        return fail(CDC_STEP_SET_CONFIG, status);

    if (ncm) {
        status = cdc_control(0x01, 0x0B, 0, layout.data_interface, 0, 0);
        if (!NT_SUCCESS(status))
            return fail(CDC_STEP_SET_INTERFACE, status);
        if (ncm_setup())
            return net_stats.step;
    }

    status = cdc_control(0x01, 0x0B, layout.data_alt, layout.data_interface, 0, 0);
    if (!NT_SUCCESS(status))
        return fail(CDC_STEP_SET_INTERFACE, status);

    ep_in = usb_open(USB_TYPE_BULK, layout.ep_in, layout.in_max, 0);
    ep_out = usb_open(USB_TYPE_BULK, layout.ep_out, layout.out_max, 0);
    if (layout.ep_intr)
        ep_intr = usb_open(USB_TYPE_INTERRUPT, layout.ep_intr, layout.intr_max,
                           layout.intr_interval);
    cdc_stats.ep_in = (u32)(unsigned long)ep_in;
    cdc_stats.ep_out = (u32)(unsigned long)ep_out;
    cdc_stats.ep_intr = (u32)(unsigned long)ep_intr;
    if (!ep_in || !ep_out)
        return fail(CDC_STEP_ENDPOINTS, usb_stats.last_open_status);

    /* PROMISCUOUS | ALL_MULTICAST | DIRECTED | BROADCAST */
    status = cdc_control(0x21, 0x43, 0x000F, layout.comm_interface, 0, 0);
    if (!NT_SUCCESS(status))
        return fail(CDC_STEP_FILTER, status);

    KeInitializeDpc(&rx_dpc, rx_dpc_routine, 0);
    rx_head = rx_tail = 0;

    tx_next = 0;
    tx_inflight = 0;
    ntb_slot = NONE;
    for (i = 0; i < tx_count; i++)
        tx_urb[i].Busy = 0;
    net_stats.tx_inflight = 0;

    running = 1;
    mem_sync();
    for (i = 0; i < rx_count; i++)
        rx_queue(&rx_urb[i]);
    if (ep_intr)
        intr_queue();

    net_stats.step = CDC_STEP_OK;
    return 0;
}

/* From RemoveDevice, at DISPATCH_LEVEL: stop resubmitting; the close chain that
 * follows completes whatever is still queued. */
void cdc_detached(void)
{
    running = 0;
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
    "cdc", cdc_match, cdc_start, cdc_detached, cdc_xmit, cdc_tx_space, 0
};
