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

/* xam -> NicXmit(user, queue, frame, length, cookie). Send over USB when this is
 * our user and the card has a link; otherwise the kernel's own EMAC path. */
static void nic_xmit_hook(void *user, u32 queue, void *frame, u32 length,
                          void *cookie)
{
    if (user == title_user && net_link_up()) {
        xnet_stats.xmit_usb++;
        net_tx((const u8 *)frame, length);
        /* xnet frees the frame once xmit "completes"; the kernel's NicXmit calls
         * the user's XmitCompleteCallback for that, so we must too. Its slot is
         * AttachInfo.XmitCompleteCallback at +0x30, context at +0x28. */
        {
            u8 *u = (u8 *)user;
            void (*done)(void *, void *) = *(void (**)(void *, void *))(u + 0x30);
            if (done)
                done(*(void **)(u + USER_CTX_OFF), cookie);
        }
        return;
    }
    xnet_stats.xmit_fallback++;
    NicXmit(user, queue, frame, length, cookie);
}

/* xam -> NicGetLinkState(deviceFlags). Report the card's link for the USB
 * interface; anything else is the kernel's answer. */
static u32 nic_link_state_hook(u32 device_flags)
{
    xnet_stats.link_polls++;
    if (device_flags & NIC_IFACE_USBENET) {
        if (net_link_up())
            return LINK_UP_VALUE;
        return 0;
    }
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
    (void)up;
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
    mem_sync();

    thunk_restore(THUNK_XMIT, saved_xmit_lis, saved_xmit_addi);
    thunk_restore(THUNK_LINK_STATE, saved_link_lis, saved_link_addi);
    *(volatile u32 *)(unsigned long)IFACE_FLAGS = saved_flags;

    xnet_stats.patched = 0;
    xnet_stats.enabled = 0;
    notify_link();                      /* back to the EMAC's link */
    title_user = 0;
}
