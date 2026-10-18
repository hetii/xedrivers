/* The RTL8156 as an Ethernet frame pipe, over CDC ECM or CDC NCM.
 *
 * Bring-up, all from the worker thread at PASSIVE_LEVEL, each control transfer
 * waited for:
 *   GET_DESCRIPTOR device, then every configuration -- pick the one for the
 *   transport xe_config asks for (cdc.c), ECM (3) or NCM (2) on this card
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
 *     frame is a transfer or ten share one (net_tx_eager 1: 12293 NTBs carried
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
 * NCM transmit aggregates adaptively: with fewer than `net_tx_eager` blocks in
 * flight the open block goes out at once (latency); otherwise frames are added to
 * it and it goes when a block completes or it fills (throughput). Every submit is
 * made after the transmit lock is released: a transfer that completed
 * synchronously would otherwise re-enter tx_complete and deadlock on it.
 *
 * Measured: more than about 100 requests queued at once on this console hangs it
 * (RX 64 + TX 128 did, every time; 64 + 64 does not), so the defaults stay well
 * under that.
 */

#include "net.h"
#include "usb.h"
#include "cdc.h"
#include "config.h"

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
#define REG_SIZE   64

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define RX_SLOTS   MAX(RX_COUNT, NCM_RX_COUNT)
#define TX_SLOTS   MAX(TX_COUNT, NCM_TX_COUNT)
#define RX_AREA    MAX(RX_COUNT * FRAME_SIZE, NCM_RX_COUNT * NTB_SIZE)
#define TX_AREA    MAX(TX_COUNT * FRAME_SIZE, NCM_TX_COUNT * NTB_SIZE)
#define FIXED_AREA (CTRL_SIZE + INTR_SIZE + REG_SIZE + 3 * 2048)
#define DMA_BYTES  (FIXED_AREA + RX_AREA + TX_AREA + 4096)

#define RX_ERROR_LIMIT   64     /* consecutive failures before the ring stops */
#define INTR_ERROR_LIMIT 16
#define NONE             0xFFFFFFFFu

volatile NET_RX net_rx;
volatile NET_LINK net_link_cb;
volatile NET_TX_IDLE net_tx_idle;
volatile u32 net_tx_eager = 2;
volatile u32 net_ncm_ndp_end = 1;
u8 net_mac[ETH_ALEN];
volatile NET_STATS net_stats;

static u8 *dma;
static u32 dma_used;
static u32 fixed_end;

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
static u32 link_down_seen;

static u8 *ctrl_buf;
static u8 *reg_buf;

static volatile u32 running;

static u16 le16(const u8 *p)
{
    return (u16)(p[0] | (p[1] << 8));
}

/* Aligned to 2048, so a frame buffer never crosses a 4 KB page whatever the
 * allocator hands back; NTB buffers are larger and rely on the block being
 * physically contiguous, which MmAllocatePhysicalMemory gives. */
void *net_dma_alloc(u32 bytes)
{
    u8 *block;

    dma_used = (dma_used + 2047u) & ~2047u;
    if (!dma || dma_used + bytes > DMA_BYTES)
        return 0;
    block = dma + dma_used;
    dma_used += bytes;
    return block;
}

void *net_reg_buffer(void)
{
    return reg_buf;
}

u32 net_link_up(void)
{
    return running && net_stats.link;
}

u32 net_speed_mbps(void)
{
    return net_stats.speed_mbps;
}

static void set_link(u32 up)
{
    if (net_stats.link == up)
        return;
    net_stats.link = up;
    if (net_link_cb)
        net_link_cb(up);
}

static void add64(volatile u32 *lo, volatile u32 *hi, u32 n)
{
    u32 before = *lo;

    *lo = before + n;
    if (*lo < before)
        *hi = *hi + 1;
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
    if (length < ETH_HLEN || length > ETH_FRAME_MAX + 4) {
        net_stats.rx_short++;
        return;
    }
    net_stats.rx_frames++;
    add64(&net_stats.rx_bytes, &net_stats.rx_bytes_hi, length);
    /* A device that never sends NETWORK_CONNECTION still delivers frames; one
     * arriving is as good a sign of link as any. */
    if (!net_stats.link && !link_down_seen)
        set_link(1);
    if (net_rx)
        net_rx(frame, length);
}

static void rx_complete(URB *urb, u32 status)
{
    if (!running || !usb.active) {
        urb->Busy = 0;
        return;
    }

    if (NT_SUCCESS(status)) {
        rx_consecutive_errors = 0;
        if (ncm) {
            net_stats.rx_ntbs++;
            if (cdc_ncm_parse_ntb(rx_buf[urb->Tag], urb->Actual, deliver, 0) < 0)
                net_stats.rx_ntb_errors++;
        } else {
            deliver(rx_buf[urb->Tag], urb->Actual, 0);
        }
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
    net_stats.tx_ntbs++;
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
                     net_ncm_ndp_end);
    return 1;
}

static void tx_complete(URB *urb, u32 status)
{
    u32 irql, send = NONE, i = urb->Tag;

    irql = KfAcquireSpinLock(&tx_lock);
    if (NT_SUCCESS(status)) {
        net_stats.tx_frames += tx_frames_in[i];
        add64(&net_stats.tx_bytes, &net_stats.tx_bytes_hi, urb->Length);
    } else {
        net_stats.tx_errors++;
        net_stats.tx_last_error = status;
    }
    tx_inflight--;
    net_stats.tx_inflight = tx_inflight;
    urb->Busy = 0;
    if (ncm && tx_inflight < net_tx_eager)
        send = ntb_close_locked();
    KfReleaseSpinLock(&tx_lock, irql);

    tx_send(send);
    if (net_tx_idle)
        net_tx_idle();
}

u32 net_tx_space(void)
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
    for (n = 0; n < length; n++)
        dst[n] = frame[n];
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
    if (tx_inflight < net_tx_eager)
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

u32 net_tx(const u8 *frame, u32 length)
{
    if (!running || !usb.active || length < ETH_HLEN || length > ETH_FRAME_MAX) {
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
            net_stats.intr_events++;
            net_stats.last_notify[0] = (u32)intr_buf[at + 1] << 16 |
                                       le16(intr_buf + at + 2);
            if (note.kind == CDC_NOTE_LINK) {
                if (!note.link_up)
                    link_down_seen = 1;
                set_link(note.link_up);
            } else if (note.kind == CDC_NOTE_SPEED) {
                net_stats.last_notify[1] = note.down_bps;
                net_stats.speed_mbps = note.down_bps / 1000000u;
            }
        }
    } else {
        net_stats.intr_errors++;
        if (++intr_consecutive_errors > INTR_ERROR_LIMIT) {
            urb->Busy = 0;
            return;
        }
    }
    intr_queue();
}

/* --- bring-up ---------------------------------------------------------------- */

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

static u32 fail(u32 step, u32 status)
{
    net_stats.step = step;
    net_stats.step_status = status;
    return step;
}

/* The fixed part once; the rings are carved again on every bring-up, because ECM
 * and NCM want different counts and sizes out of the same block. */
static u32 alloc_buffers(void)
{
    u32 i;

    if (!dma) {
        dma = (u8 *)MmAllocatePhysicalMemory(2, DMA_BYTES, 4);  /* as CUsbEnet */
        if (!dma)
            return 0;
        dma_used = 0;
        ctrl_buf = (u8 *)net_dma_alloc(CTRL_SIZE);
        intr_buf = (u8 *)net_dma_alloc(INTR_SIZE);
        reg_buf = (u8 *)net_dma_alloc(REG_SIZE);
        fixed_end = dma_used;
    }
    dma_used = fixed_end;

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

    status = control(0x80, 0x06, 0x0100, 0, 18, &actual);
    if (!NT_SUCCESS(status) || actual < 18)
        return fail(NET_STEP_DEVICE_DESC, status);
    configs = ctrl_buf[17];

    for (index = 0; index < configs; index++) {
        status = control(0x80, 0x06, (u16)(0x0200 | index), 0, 9, &actual);
        if (!NT_SUCCESS(status) || actual < 9)
            return fail(NET_STEP_CONFIG_DESC, status);
        total = le16(ctrl_buf + 2);
        if (total > CTRL_SIZE)
            continue;
        status = control(0x80, 0x06, (u16)(0x0200 | index), 0, (u16)total, &actual);
        if (!NT_SUCCESS(status))
            return fail(NET_STEP_CONFIG_DESC, status);
        if (cdc_parse_config(ctrl_buf, actual, &layout) && layout.subclass == subclass)
            return 0;
    }
    return fail(NET_STEP_LAYOUT, configs);
}

/* GET_NTB_PARAMETERS, then ask for input blocks no larger than our buffers. The
 * second is optional in the spec, so its failure is recorded and lived with --
 * unless the device's own size is larger than a buffer, which would babble. */
static u32 ncm_setup(void)
{
    u32 status, actual, want = NTB_SIZE;

    status = control(0xA1, 0x80, 0, layout.comm_interface, 28, &actual);
    if (!NT_SUCCESS(status) || !cdc_ncm_parse_params(ctrl_buf, actual, &ncm_params))
        return fail(NET_STEP_NTB_PARAMS, status);
    net_stats.ntb_in_max = ncm_params.in_max;
    net_stats.ntb_out_max = ncm_params.out_max;
    net_stats.ntb_out_align = (u32)ncm_params.out_divisor << 16 |
                              ncm_params.out_remainder;
    net_stats.ntb_out_datagrams = ncm_params.out_max_datagrams;

    net_stats.ntb_formats = ncm_params.formats;

    /* What Linux cdc_ncm sends next, all while the data interface is at alt 0,
     * all optional in the spec and so recorded rather than required: NTB16 when
     * the device also does NTB32, no CRC appended, and the largest datagram
     * from the Ethernet functional descriptor. A device left expecting CRCs or
     * NTB32 accepts our NTB16/NCM0 blocks and transmits nothing. */
    net_stats.ntb_format_status = 0xFFFFFFFFu;
    if (ncm_params.formats & 2)
        net_stats.ntb_format_status = control(0x21, 0x84, 0, layout.comm_interface,
                                              0, 0);
    net_stats.crc_status = control(0x21, 0x8A, 0, layout.comm_interface, 0, 0);
    if (layout.max_segment) {
        ctrl_buf[0] = (u8)layout.max_segment;
        ctrl_buf[1] = (u8)(layout.max_segment >> 8);
        net_stats.dgram_status = control(0x21, 0x88, 0, layout.comm_interface, 2, 0);
    }

    if (ncm_params.in_max != want) {
        ctrl_buf[0] = (u8)want;
        ctrl_buf[1] = (u8)(want >> 8);
        ctrl_buf[2] = (u8)(want >> 16);
        ctrl_buf[3] = (u8)(want >> 24);
        status = control(0x21, 0x86, 0, layout.comm_interface, 4, 0);
        net_stats.ntb_input_status = status;
        if (!NT_SUCCESS(status) && ncm_params.in_max > NTB_SIZE)
            return fail(NET_STEP_NTB_PARAMS, status);
    }
    return 0;
}

u32 net_start(void)
{
    u32 i, status, actual;

    running = 0;
    net_stats.ready = 0;
    net_stats.link = 0;
    link_down_seen = 0;
    rx_consecutive_errors = 0;
    intr_consecutive_errors = 0;
    ep_in = ep_out = ep_intr = 0;
    ncm = xe_config.transport == XE_TRANSPORT_NCM;
    net_stats.transport = ncm;

    if (!alloc_buffers())
        return fail(NET_STEP_DMA, 0);

    if (find_config(ncm ? CDC_SUBCLASS_NCM : CDC_SUBCLASS_ECM))
        return net_stats.step;
    net_stats.config_value = layout.config_value;
    net_stats.data_interface = layout.data_interface;

    if (layout.mac_string) {
        status = control(0x80, 0x06, (u16)(0x0300 | layout.mac_string), 0x0409, 255,
                         &actual);
        if (!NT_SUCCESS(status) || !cdc_parse_mac(ctrl_buf, actual, net_mac))
            return fail(NET_STEP_MAC, status);
    }

    status = control(0x00, 0x09, layout.config_value, 0, 0, 0);
    if (!NT_SUCCESS(status))
        return fail(NET_STEP_SET_CONFIG, status);

    if (ncm) {
        status = control(0x01, 0x0B, 0, layout.data_interface, 0, 0);
        if (!NT_SUCCESS(status))
            return fail(NET_STEP_SET_INTERFACE, status);
        if (ncm_setup())
            return net_stats.step;
    }

    status = control(0x01, 0x0B, layout.data_alt, layout.data_interface, 0, 0);
    if (!NT_SUCCESS(status))
        return fail(NET_STEP_SET_INTERFACE, status);

    ep_in = usb_open(USB_TYPE_BULK, layout.ep_in, layout.in_max, 0);
    ep_out = usb_open(USB_TYPE_BULK, layout.ep_out, layout.out_max, 0);
    if (layout.ep_intr)
        ep_intr = usb_open(USB_TYPE_INTERRUPT, layout.ep_intr, layout.intr_max,
                           layout.intr_interval);
    net_stats.ep_in = (u32)(unsigned long)ep_in;
    net_stats.ep_out = (u32)(unsigned long)ep_out;
    net_stats.ep_intr = (u32)(unsigned long)ep_intr;
    if (!ep_in || !ep_out)
        return fail(NET_STEP_ENDPOINTS, usb_stats.last_open_status);

    /* PROMISCUOUS | ALL_MULTICAST | DIRECTED | BROADCAST */
    status = control(0x21, 0x43, 0x000F, layout.comm_interface, 0, 0);
    if (!NT_SUCCESS(status))
        return fail(NET_STEP_FILTER, status);

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

    net_stats.step = NET_STEP_OK;
    net_stats.ready = 1;
    return 0;
}

/* From RemoveDevice, at DISPATCH_LEVEL: stop resubmitting; the close chain that
 * follows completes whatever is still queued. */
void net_detached(void)
{
    running = 0;
    net_stats.ready = 0;
    mem_sync();
    set_link(0);
}
