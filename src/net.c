/* The RTL8156 as a CDC ECM frame pipe.
 *
 * Bring-up, all from the worker thread at PASSIVE_LEVEL, each control transfer
 * waited for:
 *   GET_DESCRIPTOR device, then every configuration -- pick the ECM one (cdc.c)
 *   GET_DESCRIPTOR string iMACAddress       the card's own address
 *   SET_CONFIGURATION <ecm>                 the card's firmware takes over MAC/PHY
 *   SET_INTERFACE data, alt 1               the alt setting with the bulk pair
 *   open bulk IN, bulk OUT, interrupt IN
 *   SET_ETHERNET_PACKET_FILTER              promiscuous: whoever consumes frames
 *                                           filters them by destination
 * then RX_COUNT bulk IN requests stay queued for good, each resubmitted from its
 * own completion, and one interrupt IN request carries link notifications.
 *
 * In ECM one bulk transfer is one frame: the device ends it with a short packet,
 * or a zero-length one when the frame is a multiple of 512. On the way out we do
 * not send zero-length packets; a frame of exactly n*512 bytes gets one byte of
 * padding instead, which is what Linux usbnet does for devices without ZLP, and
 * the receiver ignores it because the IP length says where the payload ends.
 */

#include "net.h"
#include "usb.h"
#include "cdc.h"

#define RX_COUNT   32
#define RX_SIZE    2048         /* a multiple of 512, so a frame never babbles */
#define TX_COUNT   64
#define TX_SIZE    2048
#define CTRL_SIZE  512
#define INTR_SIZE  64
#define DMA_BYTES  (RX_COUNT * RX_SIZE + TX_COUNT * TX_SIZE + CTRL_SIZE + INTR_SIZE \
                    + 4096)

#define RX_ERROR_LIMIT   64     /* consecutive failures before the ring stops */
#define INTR_ERROR_LIMIT 16

volatile NET_RX net_rx;
volatile NET_LINK net_link_cb;
volatile NET_TX_IDLE net_tx_idle;
u8 net_mac[ETH_ALEN];
volatile NET_STATS net_stats;

static u8 *dma;
static u32 dma_used;

static CDC_LAYOUT layout;
static void *ep_in;
static void *ep_out;
static void *ep_intr;

static URB rx_urb[RX_COUNT];
static u8 *rx_buf[RX_COUNT];
static u32 rx_consecutive_errors;

static URB tx_urb[TX_COUNT];
static u8 *tx_buf[TX_COUNT];
static u32 tx_next;
static KSPIN_LOCK tx_lock;

static URB intr_urb;
static u8 *intr_buf;
static u32 intr_consecutive_errors;
static u32 link_down_seen;

static u8 *ctrl_buf;

static volatile u32 running;

static u16 le16(const u8 *p)
{
    return (u16)(p[0] | (p[1] << 8));
}

/* Aligned to 2048, so a transfer buffer never crosses a 4 KB page whatever the
 * allocator hands back. */
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
    usb_prepare(urb, ep_in, rx_complete, rx_buf[urb->Tag], RX_SIZE, 0);
    usb_submit(urb);
}

static void rx_complete(URB *urb, u32 status)
{
    u32 n;

    if (!running || !usb.active) {
        urb->Busy = 0;
        return;
    }

    if (NT_SUCCESS(status)) {
        rx_consecutive_errors = 0;
        n = urb->Actual;
        if (n >= ETH_HLEN && n <= RX_SIZE) {
            net_stats.rx_frames++;
            add64(&net_stats.rx_bytes, &net_stats.rx_bytes_hi, n);
            /* A device that never sends NETWORK_CONNECTION still delivers frames;
             * one arriving is as good a sign of link as any. */
            if (!net_stats.link && !link_down_seen)
                set_link(1);
            if (net_rx)
                net_rx(rx_buf[urb->Tag], n);
        } else {
            net_stats.rx_short++;
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

static void tx_complete(URB *urb, u32 status)
{
    if (NT_SUCCESS(status)) {
        net_stats.tx_frames++;
        add64(&net_stats.tx_bytes, &net_stats.tx_bytes_hi, urb->Length);
    } else {
        net_stats.tx_errors++;
        net_stats.tx_last_error = status;
    }
    net_stats.tx_inflight--;
    mem_sync();
    urb->Busy = 0;
    if (net_tx_idle)
        net_tx_idle();
}

u32 net_tx_space(void)
{
    return running && !tx_urb[tx_next].Busy;
}

u32 net_tx(const u8 *frame, u32 length)
{
    u32 irql, i, n;
    u8 *dst;

    if (!running || !usb.active || length < ETH_HLEN || length > ETH_FRAME_MAX) {
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
    net_stats.tx_inflight++;
    KfReleaseSpinLock(&tx_lock, irql);

    dst = tx_buf[i];
    for (n = 0; n < length; n++)
        dst[n] = frame[n];
    if (layout.out_max && (n % layout.out_max) == 0)
        dst[n++] = 0;

    usb_prepare(&tx_urb[i], ep_out, tx_complete, dst, n, 0);
    usb_submit(&tx_urb[i]);
    return 1;
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

static u32 alloc_buffers(void)
{
    u32 i;

    if (dma)
        return 1;
    dma = (u8 *)MmAllocatePhysicalMemory(2, DMA_BYTES, 4);   /* as CUsbEnet does */
    if (!dma)
        return 0;
    dma_used = 0;

    ctrl_buf = (u8 *)net_dma_alloc(CTRL_SIZE);
    intr_buf = (u8 *)net_dma_alloc(INTR_SIZE);
    for (i = 0; i < RX_COUNT; i++) {
        rx_buf[i] = (u8 *)net_dma_alloc(RX_SIZE);
        rx_urb[i].Tag = i;
    }
    for (i = 0; i < TX_COUNT; i++) {
        tx_buf[i] = (u8 *)net_dma_alloc(TX_SIZE);
        tx_urb[i].Tag = i;
    }
    return tx_buf[TX_COUNT - 1] != 0;
}

/* Every configuration the device has, until one parses as ECM. */
static u32 find_ecm(void)
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
        if (cdc_parse_config(ctrl_buf, actual, &layout) &&
            layout.subclass == CDC_SUBCLASS_ECM)
            return 0;
    }
    return fail(NET_STEP_LAYOUT, configs);
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

    if (!alloc_buffers())
        return fail(NET_STEP_DMA, 0);

    if (find_ecm())
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
    for (i = 0; i < TX_COUNT; i++)
        tx_urb[i].Busy = 0;
    net_stats.tx_inflight = 0;

    running = 1;
    mem_sync();
    for (i = 0; i < RX_COUNT; i++)
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
