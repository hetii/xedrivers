/* xnet integration; see xnet.h. All addresses are for retail 2.0.17559 with xam
 * from the 21256 recovery at 0x815F0000, read off this console's own memory.
 */

#include "xnet.h"
#include "net.h"

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

/* --- transmit with backpressure ----------------------------------------------
 *
 * A NIC tells its user a frame is sent when it is, and xnet does not reuse the
 * buffer before. Completing every frame on the spot and dropping it when the USB
 * ring was full lost ~0.3% of frames under load (1918 of 576638 in four parallel
 * FTP downloads) and TCP spent the rest of the run in retransmission timeouts:
 * one stream 157 Mbit/s, four streams 14 between them.
 *
 * So frames queue here, in order, with the buffer and cookie xnet gave us, and
 * leave for the ring as slots free up; each is completed to xnet only once it has
 * been copied into a transfer buffer. One drainer at a time, chosen by a
 * test-and-set flag rather than a spin lock: completing a frame re-enters xnet,
 * which may call NicXmit straight back into us, and a completion of our own
 * transfer runs the drain from the USB processor. Whoever finds the flag taken
 * leaves the work to its holder, who looks again after letting go. */

#define XQ_SIZE 512

typedef struct {
    void *user;
    void *frame;
    u32   length;
    void *cookie;
} XQ_ENTRY;

static XQ_ENTRY xq[XQ_SIZE];
static u32 xq_head, xq_tail;
static KSPIN_LOCK xq_lock;
static volatile u32 xq_draining;

static void xmit_done(void *user, void *cookie)
{
    u8 *u = (u8 *)user;
    void (*done)(void *, void *) = *(void (**)(void *, void *))(u + 0x30);

    /* AttachInfo.XmitCompleteCallback at +0x30, its context at +0x28. */
    if (done)
        done(*(void **)(u + USER_CTX_OFF), cookie);
}

static u32 xq_count(void)
{
    return (xq_tail + XQ_SIZE - xq_head) % XQ_SIZE;
}

static void xq_drain(void)
{
    XQ_ENTRY e;
    u32 irql, sent;

    for (;;) {
        if (__sync_lock_test_and_set(&xq_draining, 1))
            return;
        for (;;) {
            irql = KfAcquireSpinLock(&xq_lock);
            if (xq_head == xq_tail || !net_tx_space()) {
                KfReleaseSpinLock(&xq_lock, irql);
                break;
            }
            e = xq[xq_head];
            xq_head = (xq_head + 1) % XQ_SIZE;
            KfReleaseSpinLock(&xq_lock, irql);

            sent = net_tx((const u8 *)e.frame, e.length);
            if (!sent && net_link_up()) {
                /* The space hint raced a fill: put it back where it was. Only
                 * the drainer takes from the head, so this cannot reorder. */
                irql = KfAcquireSpinLock(&xq_lock);
                xq_head = (xq_head + XQ_SIZE - 1) % XQ_SIZE;
                xq[xq_head] = e;
                KfReleaseSpinLock(&xq_lock, irql);
                break;
            }
            if (!sent)
                xnet_stats.xmit_dropped++;
            xmit_done(e.user, e.cookie);
        }
        __sync_lock_release(&xq_draining);
        if (xq_head == xq_tail || !net_tx_space())
            break;
    }
}

/* Everything still waiting, completed unsent: the link went away. */
static void xq_flush(void)
{
    XQ_ENTRY e;
    u32 irql;

    for (;;) {
        irql = KfAcquireSpinLock(&xq_lock);
        if (xq_head == xq_tail) {
            KfReleaseSpinLock(&xq_lock, irql);
            return;
        }
        e = xq[xq_head];
        xq_head = (xq_head + 1) % XQ_SIZE;
        KfReleaseSpinLock(&xq_lock, irql);
        xnet_stats.xmit_dropped++;
        xmit_done(e.user, e.cookie);
    }
}

/* xam -> NicXmit(user, queue, frame, length, cookie). Queue for the card when this
 * is our user and the card has a link; otherwise the kernel's own EMAC path. */
static void nic_xmit_hook(void *user, u32 queue, void *frame, u32 length,
                          void *cookie)
{
    u32 irql, depth;

    if (user != title_user || !net_link_up()) {
        xnet_stats.xmit_fallback++;
        NicXmit(user, queue, frame, length, cookie);
        return;
    }

    xnet_stats.xmit_usb++;
    irql = KfAcquireSpinLock(&xq_lock);
    if ((xq_tail + 1) % XQ_SIZE == xq_head) {
        KfReleaseSpinLock(&xq_lock, irql);
        xnet_stats.xmit_dropped++;
        xmit_done(user, cookie);
        return;
    }
    xq[xq_tail].user = user;
    xq[xq_tail].frame = frame;
    xq[xq_tail].length = length;
    xq[xq_tail].cookie = cookie;
    xq_tail = (xq_tail + 1) % XQ_SIZE;
    depth = xq_count();
    if (depth > xnet_stats.xq_high)
        xnet_stats.xq_high = depth;
    KfReleaseSpinLock(&xq_lock, irql);
    xq_drain();
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
    if ((device_flags & NIC_IFACE_USBENET) && net_link_up())
        return LINK_UP_VALUE;
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

static void link_changed(u32 up)
{
    if (!up)
        xq_flush();
    notify_link();
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
    xq_flush();

    thunk_restore(THUNK_XMIT, saved_xmit_lis, saved_xmit_addi);
    thunk_restore(THUNK_LINK_STATE, saved_link_lis, saved_link_addi);
    *(volatile u32 *)(unsigned long)IFACE_FLAGS = saved_flags;

    xnet_stats.patched = 0;
    xnet_stats.enabled = 0;
    notify_link();                      /* back to the EMAC's link */
    title_user = 0;
}
