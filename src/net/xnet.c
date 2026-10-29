/* xnet integration; see xnet.h. All addresses are for retail 2.0.17559 with xam
 * from the 21256 recovery at 0x815F0000, read off this console's own memory.
 */

#include "xnet.h"
#include "bench.h"
#include "net.h"
#include "txq.h"
#include "file.h"

volatile XNET_STATS xnet_stats;

/* The interface id handed to the receive callback. The EMAC passes 1; CUsbEnet
 * passes 8 (NicInterfaceUsbEnet) with that bit ORed into xam's flags word. Which one
 * xnet insists on has not been settled on this console, so it can be changed while
 * running (xectl.py cmd 13 <id>). */
volatile u32 xnet_rx_iface = 0x08u;  /* NicInterfaceUsbEnet */

/* xam's import thunks: the `lis r11,hi` of each `lis/addi/mtctr/bctr`, and the
 * kernel Nic function the untouched thunk must decode to. Found by searching the
 * live xam image for the thunk that branches to each kernel address. */
#define THUNK_XMIT        0x81A73444u
#define THUNK_LINK_STATE  0x81A733C4u
#define ORIG_NIC_XMIT     0x800D2708u
#define ORIG_NIC_LINK     0x800D27C0u

/* The title's wired NIC base and the interface-available flags word xam reads;
 * bit 3 (NicInterfaceUsbEnet) is ORed in so xam offers the USB interface. */
#define G_NIC_EMAC        0x801864B8u
#define IFACE_FLAGS       0x81A86C2Cu
#define NIC_IFACE_EMAC    0x01u
#define NIC_IFACE_USBENET 0x08u

/* CNicUser, from CUsbEnet's recovered layout. */
#define USER_FILTER_OFF   0x24u
#define USER_CTX_OFF      0x28u
#define USER_RXCB_OFF     0x2Cu
#define USER_LINKCB_OFF   0x34u
#define USER_ORIG_FILTER_OFF 0x40u    /* OriginalAttachInfo.ReceiveFilterMask */
#define XNET_STD_FILTER   0x1Au

/* NIC_RECV_DEST and the filter bit each needs. */
#define DEST_UNICAST   1u
#define DEST_MULTICAST 3u
#define DEST_BROADCAST 4u

/* Link-state bits reported to the title when the card is up. There is no 2.5G bit
 * in this enum and the USB2 path caps well below a gigabit anyway, so the card is
 * reported as a gigabit full-duplex link with negotiation complete. */
#define LINK_UP_VALUE  0x00010049u    /* ACTIVE|FULL_DUPLEX|1000_MBPS|NEG_COMPLETE */

typedef void (*RX_CALLBACK)(void *context, void *frame, u32 length, u32 interface,
                            u32 dest);

static u32 saved_xmit_lis, saved_xmit_addi;
static u32 saved_link_lis, saved_link_addi;
static u32 saved_flags;
static void *title_user;

/* Build/decode the `lis r11,hi` + `addi r11,r11,lo` pair, with the sign extension
 * addi applies to the low half compensated in the high half. */
static u32 thunk_target(u32 lis, u32 addi)
{
    u32 hi = lis & 0xFFFF;
    s64 lo = (s16)(addi & 0xFFFF);
    return (u32)((hi << 16) + lo);
}

static void thunk_set(u32 at, void *target)
{
    u32 a = (u32)(unsigned long)target;
    u32 hi = (a + 0x8000u) >> 16;
    u32 lo = a & 0xFFFFu;

    *(volatile u32 *)(unsigned long)at = 0x3D600000u | (hi & 0xFFFF);        /* lis r11,hi  */
    *(volatile u32 *)(unsigned long)(at + 4) = 0x396B0000u | lo;             /* addi r11,.. */
    KeSweepIcacheRange((void *)(unsigned long)at, 8);
}

static void thunk_restore(u32 at, u32 lis, u32 addi)
{
    *(volatile u32 *)(unsigned long)at = lis;
    *(volatile u32 *)(unsigned long)(at + 4) = addi;
    KeSweepIcacheRange((void *)(unsigned long)at, 8);
}

/* The one title user already attached to the EMAC at boot: the first plausible
 * entry on g_NicEmac's list whose standard receive filter is set. */
static void *find_title_user(void)
{
    LIST_ENTRY *head = (LIST_ENTRY *)(unsigned long)G_NIC_EMAC;
    LIST_ENTRY *link;
    u32 guard;

    if (!MmIsAddressValid(head))
        return 0;
    link = head->Flink;
    for (guard = 0; link && link != head && guard < 16; guard++) {
        u8 *user = (u8 *)link;      /* NicBaseListEntry is at +0x00 */
        LIST_ENTRY *next;

        if (!MmIsAddressValid(user) || !MmIsAddressValid(user + 0x44))
            return 0;
        next = ((LIST_ENTRY *)user)->Flink;
        if (*(u32 *)(user + USER_ORIG_FILTER_OFF) == XNET_STD_FILTER ||
            *(u32 *)(user + USER_FILTER_OFF) == XNET_STD_FILTER)
            return user;
        link = next;
    }
    return 0;
}

/* --- the hooks --------------------------------------------------------------- */

/* --- transmit: through the queue (txq.h), to the uplink ------------------- */

static void xmit_done(void *user, void *cookie)
{
    u8 *u = (u8 *)user;
    void (*done)(void *, void *) = *(void (**)(void *, void *))(u + 0x30);

    /* AttachInfo.XmitCompleteCallback at +0x30, its context at +0x28. */
    if (done)
        done(*(void **)(u + USER_CTX_OFF), cookie);
}

static u32 xq_send(void *owner, const u8 *frame, u32 length)
{
    (void)owner;
    return net_tx(frame, length);
}

static u32 xq_space(void *owner)
{
    (void)owner;
    return net_tx_space();
}

static u32 xq_keep(void *owner)
{
    (void)owner;
    return net_link_up();
}

static void xq_done(void *owner, void *user, void *cookie)
{
    (void)owner;
    xmit_done(user, cookie);
}

static NET_TXQ xq = { .send = xq_send, .space = xq_space, .keep = xq_keep,
                      .done = xq_done, .dropped = &xnet_stats.xmit_dropped,
                      .high = &xnet_stats.xq_high };

static void xq_drain(void)
{
    net_txq_drain(&xq);
}

/* Frames waiting and none leaving for XQ_STALL_TICKS of the worker: the uplink
 * takes nothing (a card that refuses frames without having any transfer out), and
 * everything xnet sends -- to the onboard port too -- would wait behind them. Says
 * so once; net.c then takes the uplink's link down and xnet falls back. Thread. */
#define XQ_STALL_TICKS  60              /* ~2 s */

u32 xnet_tick_stalled(void)
{
    static u32 seen_head, quiet;
    u32 head = xq.head, tail = xq.tail;

    if (!xnet_stats.enabled || head == tail || head != seen_head) {
        seen_head = head;
        quiet = 0;
        return 0;
    }
    if (++quiet != XQ_STALL_TICKS)
        return 0;
    xnet_stats.xq_stalls++;
    xnet_stats.xq_stall_depth = (tail + NET_TXQ_SIZE - head) % NET_TXQ_SIZE;
    return 1;
}

/* xam -> NicXmit(user, queue, frame, length, cookie). Queue for the card when this
 * is our user and the card has a link; otherwise the kernel's own EMAC path. */
static void nic_xmit_hook(void *user, u32 queue, void *frame, u32 length,
                          void *cookie)
{
    u32 i;

    /* The address xnet sends from is the console's own: a Wi-Fi card has to
     * associate on it (aic_wifi.c), a wired one only passes it through. */
    if (user == title_user && !net_console_mac_known && length >= ETH_HLEN) {
        for (i = 0; i < ETH_ALEN; i++)
            net_console_mac[i] = ((const u8 *)frame)[ETH_ALEN + i];
        mem_sync();
        net_console_mac_known = 1;
    }
    if (user != title_user || !net_link_up()) {
        xnet_stats.xmit_fallback++;
        NicXmit(user, queue, frame, length, cookie);
        return;
    }

    xnet_stats.xmit_usb++;
    bench_frame_out((const u8 *)frame, length);
    net_txq_put(&xq, user, frame, length, cookie);
}

/* xam -> NicGetLinkState(deviceFlags). The card's link while it has one; without
 * it, the onboard EMAC's, so that unplugging the card (or its cable, or a phone
 * ending its tethering) leaves the console on its own port rather than with no
 * network at all. Transmit already falls back the same way (nic_xmit_hook) and the
 * EMAC hands its frames to the title itself, so the link answer is all that was
 * missing; every change of the card's link is notified (link_changed), and xnet
 * asks again and runs DHCP on whichever port answers. */
static u32 nic_link_state_hook(u32 device_flags)
{
    xnet_stats.link_polls++;
    if (bench_wired_hidden())
        return NicGetLinkState(device_flags & ~(NIC_IFACE_USBENET | NIC_IFACE_EMAC));
    if ((device_flags & NIC_IFACE_USBENET) && net_link_up())
        return LINK_UP_VALUE;
    if ((device_flags & NIC_IFACE_USBENET) && net_switching)
        return 0;
    if (device_flags & NIC_IFACE_USBENET)
        xnet_stats.link_fallback++;
    return NicGetLinkState(device_flags & ~NIC_IFACE_USBENET);
}

/* --- receive: hand a frame off the card up to the title ---------------------- */

static void deliver(const u8 *frame, u32 length)
{
    u8 *user = (u8 *)title_user;
    RX_CALLBACK cb;
    void *ctx;
    u32 filter, dest;

    if (!user || !MmIsAddressValid(user)) {
        xnet_stats.rx_no_user++;
        return;
    }
    if (frame[0] & 1)
        dest = (frame[0] == 0xFF && frame[1] == 0xFF) ? DEST_BROADCAST
                                                      : DEST_MULTICAST;
    else
        dest = DEST_UNICAST;

    filter = *(u32 *)(user + USER_FILTER_OFF);
    if ((filter & (1u << dest)) == 0) {
        xnet_stats.rx_filtered++;
        return;
    }

    cb = *(RX_CALLBACK *)(user + USER_RXCB_OFF);
    ctx = *(void **)(user + USER_CTX_OFF);
    if (!cb) {
        xnet_stats.rx_no_user++;
        return;
    }
    xnet_stats.rx_delivered++;
    bench_frame_in(frame, length);
    cb(ctx, (void *)frame, length, xnet_rx_iface, dest);
}

/* Tell the title's xnet that the link changed: it then asks NicGetLinkState again
 * and, on a new link, starts over with DHCP. The EMAC makes this call from its DPC
 * with its lock held, so it is made here at DISPATCH_LEVEL too. */
static void notify_link(void)
{
    u8 *user = (u8 *)title_user;
    void (*cb)(void *);
    u32 old;

    if (!user || !MmIsAddressValid(user))
        return;
    cb = *(void (**)(void *))(user + USER_LINKCB_OFF);
    if (!cb)
        return;
    xnet_stats.link_notifies++;
    old = ke_irql() < DISPATCH_LEVEL ? KeRaiseIrqlToDpcLevel() : 0xFFu;
    cb(*(void **)(user + USER_CTX_OFF));
    if (old != 0xFFu)
        KfLowerIrql(old);
}

void xnet_link_changed(void)
{
    notify_link();
}

static void link_changed(u32 up)
{
    if (!up)
        net_txq_flush(&xq);
    notify_link();
}

/* --- socket buffers ----------------------------------------------------------- */

/* xam's xnet keeps, per caller type, the socket buffer sizes a new socket starts
 * with, in a 64-byte block at the xnet object + (caller + 109) * 64; the object is
 * the context it gave the NIC for receive callbacks. A system application's
 * (caller 2: XBDM, ftpdll, dashboard) default receive buffer is 16 KB, which
 * xnet rounds to 17520 bytes on the wire, and with a 3 ms round trip over Wi-Fi
 * that caps one TCP stream into the console at under 6 MB/s. Titles get 63 KB.
 * xerunner: xnet-socket-buffer-defaults@3 */
#define XNET_BLOCK(caller)      (((caller) + 109u) * 64u)
#define XNET_SYSTEM             2u
#define XNET_DEF_RCVBUF_KB      50u
#define XNET_DEF_SNDBUF_KB      51u
#define XNET_MAX_SNDBUF_KB      53u
#define XNET_MAX_RCVBUF_KB      54u
#define XNET_RCVBUF_STOCK       16u
#define XNET_SETTINGS           "xedrivers.ini"

/* A size in KB from `key =` in XNET_SETTINGS, `fallback` without one. */
static u32 setting_kb(const char *key, u32 fallback)
{
    char value[8];
    u32 n, i, kb = 0;

    n = file_setting(XNET_SETTINGS, key, value, sizeof(value));
    if (!n)
        return fallback;
    for (i = 0; i < n && value[i] >= '0' && value[i] <= '9'; i++)
        kb = kb * 10 + (u32)(value[i] - '0');
    return kb;
}

/* `xnet_rcvbuf_kb =` and `xnet_sndbuf_kb =` in XNET_SETTINGS: the default receive
 * and send buffers of system sockets, in KB (at most 255); 0 or none leaves xnet's
 * own. Above 63 the peer has to offer window scaling, which xnet then answers
 * (shift 1 measured); 64 is refused, since a buffer of exactly 65536 makes xnet
 * advertise a zero window. A send buffer above the largest allowed raises that
 * too. Measured over the RTL8822BU: one TCP stream into the console 8.1 MB/s at
 * 16 KB, 16.1 at 63, 18.4 at 128, 20.7 at 255. But xnet's pool for system sockets
 * holds about 270 KB of data in all, sent and received together (measured: four
 * paused connections take 64 KB each, a fifth 14 KB, the rest nothing), and xnet
 * has no SACK: once data that waits for a missing segment, or for its ACK, fills
 * the pool, the segment that would free it is dropped too and every connection
 * stops for good. Over the wire that never came; over Wi-Fi it did, first through
 * frames our reorder window dropped (fixed, reorder.c), and still with both
 * directions busy: 4 connections each way at 128 KB stopped, at 32 KB and at the
 * stock 16 they did not, nor did 8 in with the reader stopped for 3 s. Hence 32 KB
 * (RCVBUF_DEFAULT_KB): one stream in 6.4 -> 12.8 MB/s, four in alongside four out
 * 4.5 -> 7.3; `xnet_rcvbuf_kb = 0` keeps xnet's 16. A program that runs few
 * connections can ask for more on its listening socket (8 in at 128 KB: 24.5 MB/s).
 * Sending is not limited by it (128 and 255 KB moved nothing), nor is ftpdll,
 * which picks its own buffers, nor XBDM, whose listener exists before this runs
 * and passes its 16 KB to what it accepts. Thread. */
#define RCVBUF_DEFAULT_KB   32u
static void set_buffers(void)
{
    u8 *xnet = *(u8 **)((u8 *)title_user + USER_CTX_OFF);
    u8 *block;
    u32 kb;

    if (!xnet || !MmIsAddressValid(xnet + XNET_BLOCK(XNET_SYSTEM) + 63))
        return;
    block = xnet + XNET_BLOCK(XNET_SYSTEM);
    xnet_stats.rcvbuf_before = block[XNET_DEF_RCVBUF_KB];
    xnet_stats.sndbuf_before = block[XNET_DEF_SNDBUF_KB];
    /* only a block that looks like the one measured: 16 KB default, 255 KB most */
    if (block[XNET_DEF_RCVBUF_KB] != XNET_RCVBUF_STOCK || block[XNET_MAX_RCVBUF_KB] != 255)
        return;
    kb = setting_kb("xnet_rcvbuf_kb", RCVBUF_DEFAULT_KB);
    if (kb && kb != 64 && kb <= 255) {
        block[XNET_DEF_RCVBUF_KB] = (u8)kb;
        xnet_stats.rcvbuf_after = kb;
    }
    kb = setting_kb("xnet_sndbuf_kb", 0);
    if (kb && kb != 64 && kb <= 255) {
        if (kb > block[XNET_MAX_SNDBUF_KB])
            block[XNET_MAX_SNDBUF_KB] = (u8)kb;
        block[XNET_DEF_SNDBUF_KB] = (u8)kb;
        xnet_stats.sndbuf_after = kb;
    }
    mem_sync();
}

/* --- the congestion window ------------------------------------------------------
 *
 * xnet's ACK path grows a connection's congestion window (+0xE4 of the socket)
 * by an MSS in slow start and by MSS*MSS/cwnd after, and clamps it to 0xFFFF in
 * r8 (lis r11,0 / ori r8,r11,0xffff), compared by cmplwi r11,0xffff in both
 * branches: one connection never has more than 64 KB in flight, which at
 * Wi-Fi's ~4 ms round trip is ~110 Mbit/s however big its buffers.
 * `xnet_cwnd_kb =` (a multiple of 64, at most 4096; 0 leaves it) makes the clamp
 * (N << 16) | 0xFFFF: lis r11,N, and the compares against r8 (cmplw r11,r8).
 * r8's other use, a halfword decrement at 0x817664d0, keeps its low 16 bits.
 * Only if the three words are the ones of xam 17559. A window above 64 KB also
 * needs a send buffer that large: the largest allowed for system sockets and
 * titles goes up with it (xnet_sndbuf_kb sets the system default).
 * Measured over the RTL8822BU: one stream out 101 Mbit/s (12.6 MB/s) stock, 124
 * (15.5) at 128 KB, 151 (18.9) at 256 KB with a 255 KB SO_SNDBUF on the
 * listener. With the stock 63 KB send buffer the clamp changes nothing, so 256 KB
 * is the default (CWND_DEFAULT_KB, `xnet_cwnd_kb = 0` keeps the stock code): it
 * lets a program that asks for a bigger send buffer use it. What such a program
 * must keep in mind is xnet's pool for system sockets: about 270 KB of data, sent
 * and received together (set_buffers); over Wi-Fi, connections whose buffers add up
 * to more stall for good once a loss leaves segments out of order (2 x 255 KB or
 * 4 x 128 KB out did; wired, 8 x 255 KB did not).
 * xerunner: xnet-congestion-window-cap@1 */
#define CWND_DEFAULT_KB     256u
#define CWND_LIS            0x81766444u
#define CWND_CMP_SLOW       0x81766478u
#define CWND_CMP_AVOID      0x8176651Cu
#define OP_LIS_R11_0        0x3D600000u
#define OP_CMPLWI_R11_FFFF  0x2B0BFFFFu
#define OP_CMPLW_R11_R8     0x7F0B4040u
#define XNET_TITLE          1u

static void put_code(u32 at, u32 word)
{
    *(volatile u32 *)(unsigned long)at = word;
    KeSweepIcacheRange((void *)(unsigned long)at, 4);
}

static void set_cwnd(void)
{
    volatile u32 *lis = (volatile u32 *)(unsigned long)CWND_LIS;
    volatile u32 *slow = (volatile u32 *)(unsigned long)CWND_CMP_SLOW;
    volatile u32 *avoid = (volatile u32 *)(unsigned long)CWND_CMP_AVOID;
    u8 *xnet = *(u8 **)((u8 *)title_user + USER_CTX_OFF), *block;
    u32 kb = setting_kb("xnet_cwnd_kb", CWND_DEFAULT_KB), n, caller, largest;

    if (kb <= 64 || kb > 4096)
        return;
    if (*lis != OP_LIS_R11_0 || *slow != OP_CMPLWI_R11_FFFF ||
        *avoid != OP_CMPLWI_R11_FFFF) {
        xnet_stats.cwnd_refused = 1;
        return;
    }
    n = (kb + 63) / 64 - 1;
    put_code(CWND_CMP_SLOW, OP_CMPLW_R11_R8);   /* still 0xFFFF in r8: no change yet */
    put_code(CWND_CMP_AVOID, OP_CMPLW_R11_R8);
    put_code(CWND_LIS, OP_LIS_R11_0 | n);
    xnet_stats.cwnd_kb = (n + 1) * 64;

    largest = xnet_stats.cwnd_kb < 255 ? xnet_stats.cwnd_kb : 255;
    if (!xnet || !MmIsAddressValid(xnet + XNET_BLOCK(XNET_SYSTEM) + 63))
        return;
    for (caller = XNET_TITLE; caller <= XNET_SYSTEM; caller++) {
        block = xnet + XNET_BLOCK(caller);
        if (block[XNET_MAX_SNDBUF_KB] < largest)
            block[XNET_MAX_SNDBUF_KB] = (u8)largest;
    }
    mem_sync();
}

/* --- enable / disable -------------------------------------------------------- */

u32 xnet_enable(void)
{
    volatile u32 *xmit = (volatile u32 *)(unsigned long)THUNK_XMIT;
    volatile u32 *link = (volatile u32 *)(unsigned long)THUNK_LINK_STATE;
    volatile u32 *flags = (volatile u32 *)(unsigned long)IFACE_FLAGS;

    if (xnet_stats.enabled)
        return XNET_OK;

    /* Refuse unless every thunk still decodes to the kernel function it should;
     * this is what makes the hardcoded addresses safe on the wrong build. */
    if (thunk_target(xmit[0], xmit[1]) != ORIG_NIC_XMIT)
        return XNET_ERR_XMIT_THUNK;
    if (thunk_target(link[0], link[1]) != ORIG_NIC_LINK)
        return XNET_ERR_LINK_THUNK;

    title_user = find_title_user();
    if (!title_user)
        return XNET_ERR_NO_USER;
    xnet_stats.user = (u32)(unsigned long)title_user;
    set_buffers();
    set_cwnd();

    saved_xmit_lis = xmit[0];
    saved_xmit_addi = xmit[1];
    saved_link_lis = link[0];
    saved_link_addi = link[1];
    saved_flags = *flags;
    xnet_stats.flags_before = saved_flags;

    *flags = saved_flags | NIC_IFACE_USBENET;
    xnet_stats.flags_after = *flags;

    thunk_set(THUNK_XMIT, (void *)nic_xmit_hook);
    thunk_set(THUNK_LINK_STATE, (void *)nic_link_state_hook);
    xnet_stats.patched = 2;

    net_rx = deliver;
    net_link_cb = link_changed;
    net_tx_idle = xq_drain;
    mem_sync();
    xnet_stats.enabled = 1;
    notify_link();
    return XNET_OK;
}

void xnet_disable(void)
{
    if (!xnet_stats.enabled)
        return;

    net_rx = 0;
    net_link_cb = 0;
    net_tx_idle = 0;
    mem_sync();
    net_txq_flush(&xq);

    thunk_restore(THUNK_XMIT, saved_xmit_lis, saved_xmit_addi);
    thunk_restore(THUNK_LINK_STATE, saved_link_lis, saved_link_addi);
    *(volatile u32 *)(unsigned long)IFACE_FLAGS = saved_flags;

    xnet_stats.patched = 0;
    xnet_stats.enabled = 0;
    notify_link();                      /* back to the EMAC's link */
    title_user = 0;
}
