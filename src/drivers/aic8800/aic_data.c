/* The AIC8800D80's frames once it has joined a network: Ethernet frames from the
 * network layer out to the chip, 802.11 ones from the chip back up as Ethernet.
 * Separate endpoints from the command channel (aic_cmd.c): bulk 0x01 out and 0x81
 * in carry data, 0x02 and 0x82 carry messages (aicwf_parse_usb assigns the first
 * bulk pair to data and the second to messages).
 *
 * Out, as aicwf_usb_bus_txdata builds it: four header bytes (the transfer's length
 * in twelve bits, then 0x01 for data), then the 28-byte host descriptor, then the
 * frame's payload without its Ethernet header -- the addresses and ethertype go in
 * the descriptor instead and the firmware builds the 802.11 header. Padded to four
 * bytes, and one zero byte appended when that lands on a multiple of 512, so the
 * transfer never ends on a full packet with no short one to close it.
 *
 * In, as aicwf_process_rxframes walks it: packets back to back, each a 60-byte
 * hardware header whose first 16 bits are the MPDU's length, then the MPDU as
 * received -- decrypted by the chip, but still carrying its 802.11 and security
 * headers. A message may share the transfer (its third byte has 0x10 set), which
 * is why the parse is the command channel's as well.
 *
 * Every offset here was taken from the driver's own structures, compiled rather
 * than counted: hostdesc is 28 bytes (packet_len 0, status 4, destination 8,
 * source 14, ethertype 20, access category 22, TID 23, interface 24, station 25,
 * flags 26) and hw_rxhdr 56, so the MPDU starts at 60 with the padding
 * aicwf_process_rxframes allows for.
 */

#include "aic.h"
#include "wpa.h"
#include "prof.h"

#define TX_HEADER       4u
#define HOSTDESC        28u
#define TX_OVERHEAD     (TX_HEADER + HOSTDESC)
#define RX_MPDU_OFFSET  60u     /* RX_HWHRD_LEN: hw_rxhdr (56) and its padding */
#define TX_ALIGN        4u

#define TYPE_DATA       0x01u

/* hw_rxhdr, by byte: the flags word at 48 and the decryption status in the status
 * word at 36, bits 2-4. */
#define RX_FLAGS_BYTE   48u
#define RX_FLAG_AMSDU   0x01u
#define RX_FLAG_4ADDR   0x04u
#define RX_FLAG_NEED_REORDER 0x20u
#define RX_FLAG_UPLOAD  0x40u
#define RX_DECR_BYTE    36u

/* Decryption status (RWNX_RX_HD_DECR_*): how many bytes of security header sit
 * between the 802.11 header and the LLC/SNAP that follows it. */
static u32 security_header(u32 decr_status)
{
    switch (decr_status) {
    case 1:                     /* TKIP */
    case 3: case 4:             /* CCMP 128 / 256 */
    case 5: case 6:             /* GCMP 128 / 256 */
        return 8;
    case 2:                     /* WEP */
        return 4;
    case 7:                     /* WAPI */
        return 18;
    default:
        return 0;               /* no encryption */
    }
}

/* --- out ---------------------------------------------------------------------- */

static void tx_complete(URB *urb, u32 status)
{
    AIC *a = (AIC *)urb->Owner;

    PROF_SINCE(tx_wait, urb->Stamp, urb->Length);
    /* Counted up on whichever processor xnet transmits from and down on the USB
     * one: a plain ++ and -- lose one now and then, and a count that never comes
     * back to zero reads as a transmit that never finished. */
    __sync_fetch_and_sub(&a->nd->st->tx_inflight, 1);
    if (!NT_SUCCESS(status)) {
        a->st->data_tx_errors++;
        a->st->data_tx_last_error = status;
        a->nd->st->tx_errors++;
        a->nd->st->tx_last_error = status;
    }
    urb->Busy = 0;
    mem_sync();
    if (a->nd)
        net_tx_done(a->nd);
}

/* The transmit cap of the temperature watch (aic_data_tick): bytes go out as the
 * time base refills a budget at tx_cap_kbps, at most CAP_BURST ahead. */
#define TB_PER_US   50u                     /* the 50 MHz time base */
#define CAP_BURST   65536

static u32 time_base(void)
{
    u32 t = 0;

#if defined(__powerpc__)
    __asm__ volatile("mftb %0" : "=r"(t));
#endif
    return t;
}

static u32 cap_allows(AIC *a)
{
    u32 kbps = a->st->tx_cap_kbps, us;

    if (!kbps)
        return 1;
    us = (time_base() - a->cap_last) / TB_PER_US;
    if (us) {
        a->cap_last += us * TB_PER_US;
        if (us > 20000u)
            us = 20000u;
        a->cap_budget += (s32)(us * (kbps / 8u) / 1000u);
        if (a->cap_budget > CAP_BURST)
            a->cap_budget = CAP_BURST;
    }
    if (a->cap_budget > 0)
        return 1;
    a->st->tx_capped++;
    return 0;
}

u32 aic_data_tx_space(AIC *a)
{
    u32 i;

    if (!a->joined || !cap_allows(a))
        return 0;
    for (i = 0; i < AIC_DATA_TX_COUNT; i++)
        if (!a->data_tx_urb[i].Busy)
            return 1;
    return 0;
}

u32 aic_data_xmit(AIC *a, const u8 *frame, u32 length)
{
    u8 *b;
    u32 i, slot = AIC_DATA_TX_COUNT, payload, total;

    if (!a->joined || !a->dev->active || length < ETH_HLEN)
        return 0;
    /* Before the handshake is through only its own frames may go. */
    if (a->secured && wpa_stats.state != WPA_DONE &&
        (frame[12] != (u8)(WPA_ETHERTYPE >> 8) || frame[13] != (u8)WPA_ETHERTYPE))
        return 0;
    payload = length - ETH_HLEN;
    if (TX_OVERHEAD + payload + 1 > AIC_DATA_TX_SIZE)
        return 0;

    for (i = 0; i < AIC_DATA_TX_COUNT; i++)
        if (!__sync_lock_test_and_set(&a->data_tx_urb[i].Busy, 1)) {
            slot = i;
            break;
        }
    if (slot == AIC_DATA_TX_COUNT) {
        a->st->data_tx_full++;
        return 0;
    }

    b = a->data_tx_buf[slot];
    /* The host descriptor. Nothing is asked to be confirmed: a confirmation is
     * only for the frames Linux must know the fate of (management, EAPOL), and
     * the completion of the transfer is enough for us. */
    aic_put16(b + TX_HEADER + 0, payload);
    aic_put16(b + TX_HEADER + 2, 0);                /* flags_ext */
    aic_put32(b + TX_HEADER + 4, 0);                /* status_desc_addr */
    for (i = 0; i < ETH_ALEN; i++) {
        b[TX_HEADER + 8 + i] = frame[i];            /* destination */
        b[TX_HEADER + 14 + i] = frame[ETH_ALEN + i];/* source */
    }
    b[TX_HEADER + 20] = frame[12];                  /* ethertype, in wire order */
    b[TX_HEADER + 21] = frame[13];
    b[TX_HEADER + 22] = AIC_AC_BE;
    b[TX_HEADER + 23] = 0;                          /* TID 0, best effort */
    b[TX_HEADER + 24] = (u8)a->vif;
    b[TX_HEADER + 25] = (u8)a->ap_sta;
    aic_put16(b + TX_HEADER + 26, 0);               /* flags */
    for (i = 0; i < payload; i++)
        b[TX_OVERHEAD + i] = frame[ETH_HLEN + i];

    total = TX_OVERHEAD + payload;
    while (total & (TX_ALIGN - 1))
        b[total++] = 0;
    b[0] = (u8)total;
    b[1] = (u8)((total >> 8) & 0x0F);
    b[2] = TYPE_DATA;
    b[3] = 0;
    /* A transfer that is a whole number of packets leaves the chip waiting for the
     * short one that ends it; one more byte is the driver's own answer to that. */
    if (!(total % 512u))
        b[total++] = 0;

    usb_prepare(&a->data_tx_urb[slot], a->dev, a->ep_data_out, tx_complete, b, total, 0);
    a->cap_budget -= (s32)total;
    a->st->data_tx_frames++;
    a->nd->st->tx_frames++;
    __sync_fetch_and_add(&a->nd->st->tx_inflight, 1);
    net_add64(&a->nd->st->tx_bytes, &a->nd->st->tx_bytes_hi, length);
    PROF_MARK(a->data_tx_urb[slot].Stamp);
    PROF_SINCE(turnaround, a->last_rx_stamp, length);
    usb_submit(&a->data_tx_urb[slot]);
    return 1;
}

/* --- in ----------------------------------------------------------------------- */

static void rx_complete(URB *urb, u32 status);

static void rx_queue(AIC *a, URB *urb)
{
    usb_prepare(urb, a->dev, a->ep_data_in, rx_complete,
                a->data_rx_buf[urb->Tag], AIC_DATA_RX_SIZE, 0);
    usb_submit(urb);
}

static void deliver(AIC *a, u8 *eth, u32 length)
{
    PROF_MARK(a->last_rx_stamp);
    net_frame_in(a->nd, eth, length);
}

/* Block-ack windows (src/wireless/reorder.c) get the frames the firmware flags
 * for reordering and hand them back through here, in order. They are touched only
 * on the USB processor: by the receive path, and by the DPC below for a timeout. */
static void reorder_deliver(void *context, u8 *eth, u32 length)
{
    deliver((AIC *)context, eth, length);
}

static void reorder_dpc_routine(KDPC *dpc, void *context, void *a1, void *a2)
{
    AIC *a = (AIC *)context;

    (void)dpc; (void)a1; (void)a2;
    a->reorder_dpc_queued = 0;
    if (a->data_running)
        wl_reorder_expire(&a->reorder);
}

/* From the worker, every tick: a hole that has held frames back for long enough
 * is let go by the DPC, where the receive path cannot be running at the same time. */
/* The host controller can stop asking for the data receive transfers under heavy
 * sending (USB_RX_WATCH): taken back after DATA_RX_KICK_TICKS without one while
 * sending. No reset from here: the firmware passes no beacons up, so an idle
 * link can go a long while without a receive transfer. */
#define DATA_RX_KICK_TICKS  6               /* ~120 ms of 20 ms worker ticks */

/* The AIC8800D80 does not hold a sustained upload: sending flat out, its own
 * temperature reading (TEMP_COMP_GET of MM_SET_VENDOR_SWCONFIG, answered with the
 * compensation off) went from 92 to 127, as far as it reads, in about 25 s, and a
 * few seconds later the radio stopped both ways for some 40 s -- frames and the
 * access point's ACKs alike, so the access point dropped the station ("excessive
 * missing ACKs"). Receiving at 119 Mbit/s it settled at 108. The firmware's rate
 * control held VHT-MCS 9 at 94-99% success up to the stop: no fixed rate helps;
 * the temperature compensation on, or 6 dBm instead of 14-18, did not slow the
 * rise. Sending capped to 32 Mbit/s it settled at 110 for two minutes; at 45 or
 * so it reached 127 again. Hence a watch: read every TEMP_TICKS; from TEMP_HOT on
 * cap what goes out, lower it while it stays hot, raise it once cool, and lift it
 * at TEMP_CAP_MAX. A transfer of less than about 20 s goes at the full rate.
 * Measured, ten minutes of four streams out: 116/110 held 110-112 (119 at most),
 * 25-31 Mbit/s after the first two minutes, no disconnect; 120/115 reached 126
 * for 28-34 Mbit/s, too near the stop for what it adds.
 * xerunner: aic8800-throughput-findings@2 */
#define MM_SET_VENDOR_SWCONFIG_REQ  AIC_MSG(0u, 135u)
#define MM_SET_VENDOR_SWCONFIG_CFM  AIC_MSG(0u, 136u)
#define TEMP_COMP_GET_REQ   2u
#define TEMP_TICKS          50u             /* 1 s */
#define TEMP_RETRY_TICKS    500u            /* 10 s */
#define TEMP_HOT            116u
#define TEMP_COOL           110u
#define TEMP_CAP_FIRST      100000u         /* kbit/s */
#define TEMP_CAP_MIN        12000u
#define TEMP_CAP_MAX        400000u

static void temperature(AIC *a)
{
    u8 p[4] = { TEMP_COMP_GET_REQ, 0, 0, 0 };
    u32 t, cap = a->st->tx_cap_kbps;

    if (a->temp_unknown || ++a->temp_ticks < (a->temp_retry ? TEMP_RETRY_TICKS : TEMP_TICKS))
        return;
    a->temp_ticks = 0;
    if (!NT_SUCCESS(aic_send(a, MM_SET_VENDOR_SWCONFIG_REQ, p, 4,
                             MM_SET_VENDOR_SWCONFIG_CFM))) {
        /* unanswered this time (the USB host, a busy firmware): the cap stays
         * as it is and the next reading comes TEMP_RETRY_TICKS later; each
         * try holds the worker for aic_send's CFM_TIMEOUT_MS */
        a->st->temp_failures++;
        a->temp_retry = 1;
        if (!a->st->chip_temp_max && a->st->temp_failures >= 3)
            a->temp_unknown = 1;            /* never answered: taken as unknown */
        return;
    }
    a->temp_retry = 0;
    if (a->cfm_len < 6 || a->cfm[0] != TEMP_COMP_GET_REQ) {
        a->temp_unknown = 1;                /* a firmware without the reading */
        a->st->tx_cap_kbps = 0;
        return;
    }
    t = (u32)(s32)(signed char)a->cfm[5];   /* { swconfig id, status, degree } */
    a->st->chip_temp = t;
    if (t > a->st->chip_temp_max && t < 0x80000000u)
        a->st->chip_temp_max = t;
    if (t >= TEMP_HOT && t < 0x80000000u) {
        if (!cap) {
            cap = TEMP_CAP_FIRST;
            a->cap_last = time_base();
            a->cap_budget = 0;
        } else {
            cap -= cap / 4u;
        }
        if (cap < TEMP_CAP_MIN)
            cap = TEMP_CAP_MIN;
    } else if (cap && t <= TEMP_COOL) {
        cap += cap / 8u;
        if (cap >= TEMP_CAP_MAX)
            cap = 0;
    }
    if (cap && (!a->st->tx_cap_lowest || cap < a->st->tx_cap_lowest))
        a->st->tx_cap_lowest = cap;
    a->st->tx_cap_kbps = cap;
}

void aic_data_tick(AIC *a)
{
    u32 sending;

    if (!a->data_running)
        return;
    temperature(a);
    /* frames held by the cap go on: nothing else may come back to ask */
    if (a->st->tx_cap_kbps && cap_allows(a))
        net_tx_done(a->nd);
    if (!a->data_rx_watch.urbs)
        usb_rx_watch_init(&a->data_rx_watch, a->data_rx_urb, AIC_DATA_RX_COUNT,
                          DATA_RX_KICK_TICKS, 0xFFFFFFFFu, &a->st->rx_kicks,
                          &a->st->rx_kick_ok);
    sending = a->st->data_tx_frames != a->data_rx_watch_tx;
    a->data_rx_watch_tx = a->st->data_tx_frames;
    a->data_rx_watch.kick_ticks = sending ? DATA_RX_KICK_TICKS : 0;
    usb_rx_watch(&a->data_rx_watch, a->joined, a->st->data_rx_transfers);
    if (wl_reorder_tick(&a->reorder) && !__sync_lock_test_and_set(&a->reorder_dpc_queued, 1))
        KeInsertQueueDpc(&a->reorder_dpc, 0, 0);
}

/* A new association: the access point numbers its frames afresh. From the
 * indications, which arrive on the USB processor like the frames do. */
void aic_data_rejoin(AIC *a)
{
    u32 t;

    wl_reorder_reset(&a->reorder);
    for (t = 0; t < 8; t++)
        a->rx_last_seq[t] = 0;
}

/* One MPDU, converted to an Ethernet frame where it lies and handed up. The
 * Ethernet header is written into the 802.11 and LLC/SNAP headers it replaces,
 * which are always longer than it, so nothing is copied and nothing moves. */
static void mpdu_in(AIC *a, u8 *p, u32 length)
{
    const u8 *header = p - RX_MPDU_OFFSET;
    u32 hdr_len = 24, from_ds, security, strip, i, qos = 0, tid = 0, seq = 0, last;
    u8 addresses[2 * ETH_ALEN];
    u8 *eth;

    /* Data frames only: type 2 in bits 2-3 of the frame control. */
    if ((p[0] & 0x0C) != 0x08) {
        a->st->data_rx_not_data++;
        return;
    }
    if (header[RX_FLAGS_BYTE] & (RX_FLAG_AMSDU | RX_FLAG_4ADDR)) {
        /* An aggregated MSDU holds several frames behind one header and a
         * four-address frame has a longer one; neither is taken apart here. */
        a->st->data_rx_unsupported++;
        return;
    }
    if (p[0] & 0x80) {                      /* QoS data: a control field follows */
        qos = 1;
        tid = p[24] & 0x07;
        seq = (u32)(p[22] >> 4 | p[23] << 4);
        hdr_len += 2;
    }
    if (p[1] & 0x80)                        /* HT control */
        hdr_len += 4;
    from_ds = (p[1] & 0x03) == 0x02;
    if (!from_ds) {
        /* Only what came through the AP: a station sees nothing else. */
        a->st->data_rx_not_from_ds++;
        return;
    }
    security = security_header((header[RX_DECR_BYTE] >> 2) & 0x07);
    strip = hdr_len + security + 8;         /* and the LLC/SNAP that follows */
    if (length <= strip) {
        a->st->data_rx_short++;
        return;
    }

    /* In a from-DS frame the destination is address 1 and the source address 3.
     * Both are read out before anything is written, because the header being
     * built overlaps the addresses it is built from. The ethertype needs no move
     * at all: the last two bytes of the LLC/SNAP header are already where an
     * Ethernet header's belong. */
    for (i = 0; i < ETH_ALEN; i++) {
        addresses[i] = p[4 + i];
        addresses[ETH_ALEN + i] = p[16 + i];
    }
    eth = p + strip - ETH_HLEN;
    for (i = 0; i < 2 * ETH_ALEN; i++)
        eth[i] = addresses[i];
    if (eth[12] == (u8)(WPA_ETHERTYPE >> 8) && eth[13] == (u8)WPA_ETHERTYPE) {
        /* The handshake's own frames are the station's business, not the
         * console's: they arrive before any key is in place. */
        a->st->eapol_in++;
        wpa_eapol_in(eth + ETH_HLEN, length - strip);
        if (wpa_stats.pending)
            usb_wake();
        return;
    }
    if (qos) {
        last = a->rx_last_seq[tid];
        if ((last & 0x8000) && seq != (last & 0xFFF) && ((last - seq) & 0xFFF) < 0x800)
            a->st->data_rx_out_of_order++;
        else
            a->rx_last_seq[tid] = (u16)(0x8000 | seq);
    }
    if (qos && (header[RX_FLAGS_BYTE] & RX_FLAG_NEED_REORDER)) {
        wl_reorder_in(&a->reorder, tid, seq, eth, length - strip + ETH_HLEN);
        return;
    }
    if (qos)            /* not flagged: the block-ack session, if any, is over */
        wl_reorder_end(&a->reorder, tid);
    deliver(a, eth, length - strip + ETH_HLEN);
}

/* A transfer holds packets back to back: a data packet is its 60-byte header and
 * the MPDU its length names, a message packet is the command channel's and is
 * passed back to it. A zero length ends the walk. */
static void data_parse(AIC *a, u8 *p, u32 total)
{
    u32 at = 0, length, size;

    while (at + 4 <= total) {
        length = (u32)(p[at] | p[at + 1] << 8);
        if (!length)
            break;
        if (p[at + 2] & AIC_TYPE_CFG) {
            size = ((length + 3) & ~3u) + 4;
            if (at + size > total)
                break;
            aic_message_in(a, p + at, length);
        } else {
            size = length + RX_MPDU_OFFSET;
            if (at + size > total) {
                a->st->data_rx_truncated++;
                break;
            }
            a->st->data_rx_frames++;
            mpdu_in(a, p + at + RX_MPDU_OFFSET, length);
        }
        at += size;
    }
}

static void rx_complete(URB *urb, u32 status)
{
    AIC *a = (AIC *)urb->Owner;

    urb->Busy = 0;                      /* the controller is done with it */
    if (!a->data_running || !a->dev->active)
        return;
    if (!NT_SUCCESS(status)) {
        a->st->data_rx_errors++;
        a->st->data_rx_last_error = status;
        if (++a->data_rx_consecutive_errors >= AIC_RX_ERROR_LIMIT)
            return;                             /* the device is going */
        rx_queue(a, urb);
        return;
    }
    a->data_rx_consecutive_errors = 0;
    a->st->data_rx_transfers++;
    data_parse(a, a->data_rx_buf[urb->Tag], urb->Actual);
    rx_queue(a, urb);
}

/* --- the rings ----------------------------------------------------------------- */

u32 aic_data_open(AIC *a)
{
    u32 i;

    for (i = 0; i < AIC_DATA_RX_COUNT; i++) {
        a->data_rx_buf[i] = (u8 *)net_dma_alloc(a->nd, AIC_DATA_RX_SIZE);
        if (!a->data_rx_buf[i])
            return AIC_STEP_DMA;
        a->data_rx_urb[i].Tag = i;
        a->data_rx_urb[i].Owner = a;
    }
    for (i = 0; i < AIC_DATA_TX_COUNT; i++) {
        a->data_tx_buf[i] = (u8 *)net_dma_alloc(a->nd, AIC_DATA_TX_SIZE);
        if (!a->data_tx_buf[i])
            return AIC_STEP_DMA;
        a->data_tx_urb[i].Tag = i;
        a->data_tx_urb[i].Owner = a;
        a->data_tx_urb[i].Busy = 0;
    }
    a->ep_data_out = usb_open(a->dev, USB_TYPE_BULK, 0x01, 512, 0);
    a->ep_data_in = usb_open(a->dev, USB_TYPE_BULK, 0x81, 512, 0);
    if (!a->ep_data_out || !a->ep_data_in)
        return AIC_STEP_ENDPOINTS;

    if (!a->reorder_pool) {
        a->reorder_pool = (u8 *)MmAllocatePhysicalMemory(2, WL_REORDER_POOL, 4);
        usb_dpc_init(&a->reorder_dpc, reorder_dpc_routine, a);
        a->reorder_dpc.TargetNumber = USB_PROCESSOR + 1;
    }
    wl_reorder_init(&a->reorder, a->reorder_pool, reorder_deliver, a, &a->st->reorder);
    aic_data_rejoin(a);
    a->reorder_dpc_queued = 0;

    a->data_rx_consecutive_errors = 0;
    a->data_running = 1;
    mem_sync();
    for (i = 0; i < AIC_DATA_RX_COUNT; i++)
        rx_queue(a, &a->data_rx_urb[i]);
    return 0;
}
