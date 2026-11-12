/* The network layer; see net.h. Everything here is the same for every wired card:
 * which driver has each device, their DMA memory, frames handed up, link state,
 * counters, which device is the uplink, and who consumes the frames (xnet, the
 * bench's responder, nobody). How frames reach the wire is the drivers'
 * (src/drivers/).
 */

#include "net.h"
#include "usb.h"
#include "driver.h"
#include "config.h"
#include "xnet.h"
#include "bench.h"
#include "prof.h"
#include "file.h"
#include "second.h"
#include "acks.h"
#include "window.h"
#include "ipstack.h"

#define CMD_MODE       5u   /* [1] XE_MODE_* */
#define CMD_XNET_IFACE 13u  /* [1] interface id given to xnet's receive callback */

#define SCRATCH_BYTES 16384

volatile NET_RX net_rx;
volatile NET_LINK net_link_cb;
volatile NET_TX_IDLE net_tx_idle;
u8 net_mac[ETH_ALEN];
u8 net_console_mac[ETH_ALEN];
volatile u32 net_console_mac_known;
NET_DEV net_devs[NET_MAX];
volatile NET_STATS net_stats[NET_MAX];
volatile u32 net_active;

static u32 up_counter;

void net_add64(volatile u32 *lo, volatile u32 *hi, u32 n)
{
    u32 v = *lo + n;

    if (v < n)
        (*hi)++;
    *lo = v;
}

/* The secured category, and the address in it: found by reading every setting of
 * every category off the bench console and recognising the one that held its known
 * address (tools/xectl.py cmd 18, src/bench/debug). Whatever comes back is only
 * taken if it is six bytes and could be an address at all -- a unicast one that is
 * not all zeroes -- so a build against another kernel, numbering its settings
 * differently, is left no worse off than before, with xnet's own first frame still
 * to learn the address from (xnet.c). */
#define XCONFIG_SECURED         2u
#define XCONFIG_SECURED_MAC     1u

void net_learn_console_mac(void)
{
    u8 mac[ETH_ALEN];
    u16 needed = 0;
    u32 status, i, bits = 0;

    for (i = 0; i < ETH_ALEN; i++)
        mac[i] = 0;
    status = ExGetXConfigSetting(XCONFIG_SECURED, XCONFIG_SECURED_MAC, mac,
                                 ETH_ALEN, &needed);
    net_state.console_mac_status = status;
    net_state.console_mac_needed = needed;
    for (i = 0; i < ETH_ALEN; i++) {
        net_state.console_mac_read[i / 4] |= (u32)mac[i] << (24 - 8 * (i % 4));
        bits |= mac[i];
    }
    if (!NT_SUCCESS(status) || needed != ETH_ALEN || !bits || (mac[0] & 1))
        return;
    for (i = 0; i < ETH_ALEN; i++)
        net_console_mac[i] = mac[i];
    mem_sync();
    net_console_mac_known = 1;
    net_state.console_mac_from_config = 1;
}

static NET_DEV *uplink(void)
{
    u32 a = net_active;

    return a ? &net_devs[a - 1] : 0;
}

/* --- which device is the uplink -------------------------------------------- */

/* Back sockets before front, a lower port first: the controller's low bit is the
 * pair (0 the back, 1 the front and the internal connectors), bits 2-4 the port. */
static u32 socket_rank(const NET_DEV *nd)
{
    u32 where = nd->st->where;

    return (where & 1u) << 3 | ((where >> 2) & 7u);
}

/* `uplink =` in xedrivers.ini, a driver's name (rtl8156, rtw8822b, aic8800, ...): the card
 * to use whenever it has a link, whatever the others are; without it the order below. Read
 * once (net_layer_init). */
static char uplink_wanted[12];
static u32 steal_hold;                  /* worker ticks the wanted card leaves the address be */

static u32 wanted(const NET_DEV *nd)
{
    const char *a = uplink_wanted, *b = nd->drv ? nd->drv->name : "";

    if (!*a)
        return 0;
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

/* 1 if `a` should be the uplink rather than `b` (both with a link up). */
static NET_DEV *console_mac_owner;
volatile u32 net_console_mac_handovers;
volatile u32 net_tick_longest[NET_MAX];         /* each slot's driver tick, time base ticks */
volatile u32 net_stall_retries;                 /* links the stall watch took down, given back */
#define STALL_RETRY_TICKS 900u                  /* worker ticks: ~30 s */

static u32 net_tb(void)
{
#if defined(__powerpc__)
    u32 t;

    __asm__ volatile("mftb %0" : "=r"(t));
    return t;
#else
    return 0;
#endif
}
static u32 owner_unlinked;              /* worker ticks the owner has gone without a link */

/* The console's address goes to another card when its card has had no link for 30 s while
 * another wireless card has one, on its own address, which xnet cannot use (2026-11-09: the
 * RTL8822BU did not join after a boot, the AIC8800 did, and the console was unreachable).
 * The cards' bridge loops see the change and join again (bridge.c). */
#define OWNER_UNLINKED_TICKS 1500

static void owner_watch(void)
{
    u32 i;

    if (steal_hold)
        steal_hold--;
    if (!console_mac_owner || console_mac_owner->st->link) {
        owner_unlinked = 0;
        return;
    }
    if (++owner_unlinked < OWNER_UNLINKED_TICKS)
        return;
    for (i = 0; i < NET_MAX; i++) {
        NET_DEV *nd = &net_devs[i];

        if (nd != console_mac_owner && nd->drv && nd->drv->kind == XE_KIND_WIFI &&
            nd->st->ready && nd->st->link) {
            console_mac_owner = 0;
            owner_unlinked = 0;
            steal_hold = 2 * OWNER_UNLINKED_TICKS;
            net_console_mac_handovers++;
            return;
        }
    }
}

/* A cable or a phone with a link is the uplink before any wireless card (better), and it
 * carries the console's address. A wireless card joined on that address too would take it
 * in the network's forwarding tables with every frame it sends, and what is meant for the
 * console would go to a card whose frames are dropped: with an RTL8156B and an RTL8822BU
 * on one network the console answered for 5 s after a boot and then no more (2026-11-11;
 * the station removed at the access point, it answered again within 3 s). */
static u32 cable_up(void)
{
    u32 i;

    for (i = 0; i < NET_MAX; i++) {
        const NET_DEV *nd = &net_devs[i];

        if (nd->drv && nd->drv->kind > XE_KIND_WIFI && nd->drv->kind != XE_KIND_AUDIO &&
            nd->st->ready && nd->st->link && !nd->claim)
            return 1;
    }
    return 0;
}

/* The uplink's MAC, as it joined (net_join_mac), and 1 while it has a link: the own stack's
 * address (src/stack). */
u32 net_uplink_mac(u8 *mac)
{
    NET_DEV *nd = uplink();
    const u8 *m;
    u32 i;

    if (!nd || !nd->st->ready)
        return 0;
    /* a cabled card carries the console's MAC, as xnet sends with it; a Wi-Fi card is
     * associated with the one it joined with */
    m = nd->drv->kind == XE_KIND_WIFI || !net_console_mac_known ? net_join_mac(nd)
                                                                 : net_console_mac;
    for (i = 0; i < ETH_ALEN; i++)
        mac[i] = m[i];
    return nd->st->link;
}

const u8 *net_join_mac(NET_DEV *nd)
{
    if (!net_console_mac_known)
        return nd->mac;
    if (cable_up() && !wanted(nd)) {
        if (console_mac_owner == nd)
            console_mac_owner = 0;
        return nd->mac;                 /* its own, until the cable goes */
    }
    if (!console_mac_owner || console_mac_owner == nd) {
        console_mac_owner = nd;
    } else if (wanted(nd) && !wanted(console_mac_owner) && !steal_hold) {
        console_mac_owner = nd;         /* the other card's bridge sees it and joins anew */
        net_console_mac_handovers++;
    }
    return console_mac_owner == nd ? net_console_mac : nd->mac;
}

static u32 better(const NET_DEV *a, const NET_DEV *b, const NET_DEV *current)
{
    /* a wireless card not on the console's address cannot carry xnet, wanted or not */
    if (wanted(a) != wanted(b) &&
        !((wanted(a) ? a : b)->drv->kind == XE_KIND_WIFI && (wanted(a) ? a : b) != console_mac_owner))
        return wanted(a);
    if (a->drv->kind != b->drv->kind)
        return a->drv->kind > b->drv->kind;
    /* two wireless cards: xnet sends from the console's address, which only one joined on */
    if (a->drv->kind == XE_KIND_WIFI && (a == console_mac_owner) != (b == console_mac_owner))
        return a == console_mac_owner;
    if (a->st->speed_mbps != b->st->speed_mbps)
        return a->st->speed_mbps > b->st->speed_mbps;
    if (a == current || b == current)
        return a == current;
    if (a->drv->kind == XE_KIND_TETHER && a->up_order != b->up_order)
        return a->up_order < b->up_order;      /* the phone that tethered first */
    return socket_rank(a) < socket_rank(b);
}

/* From one device to another, xnet has to see the link go down in between, or it
 * keeps its address: both report the same LINK_UP_VALUE, so a down and an up
 * notified back to back are read as one unbroken link (measured: switching from
 * the RTL8156 to a phone and back, the console kept the old address until the
 * dashboard's network test). So such a switch first gives xnet no link at all --
 * not the onboard port's either -- for SWITCH_GAP_TICKS of the worker (~1.5 s),
 * then the new uplink. To or from no uplink there is no gap: the onboard port's
 * link reads differently, and xnet starts over by itself. */
#define SWITCH_GAP_TICKS 45

volatile u32 net_switching;
static u32 switch_ticks;

/* Pick the uplink again; on a change, tell the consumer -- xnet then asks for the
 * link and runs DHCP on whatever it is now (or the onboard port). Any IRQL up to
 * DISPATCH. */
static void choose_uplink(void)
{
    NET_DEV *current = uplink(), *best = 0;
    u32 i;

    if (net_switching)
        return;                 /* decided again when the gap ends (net_tick) */
    for (i = 0; i < NET_MAX; i++) {
        NET_DEV *nd = &net_devs[i];

        if (!nd->drv || !nd->st->ready || !nd->st->link || nd->claim)
            continue;
        if (!best || better(nd, best, current))
            best = nd;
    }
    if (best == current)
        return;
    bench_event(2, current ? (u32)(current - net_devs) + 1 : 0,
                best ? (u32)(best - net_devs) + 1 : 0, (u32)__builtin_return_address(0));
    if (current && best) {
        net_active = 0;
        switch_ticks = 0;
        mem_sync();
        net_switching = 1;
        if (net_link_cb)
            net_link_cb(0);
        return;
    }
    if (best) {
        for (i = 0; i < ETH_ALEN; i++)
            net_mac[i] = best->mac[i];
    }
    mem_sync();
    net_active = best ? (u32)(best - net_devs) + 1 : 0;
    if (net_link_cb)
        net_link_cb(best != 0);
}

/* Whenever a link, a claim or the uplink changes: the uplink, then the second way's card
 * (second.h), which is one of those left. */
static void choose(void)
{
    choose_uplink();
    second_choose();
}

/* --- frames and link, from the drivers -------------------------------------- */

static u32 uplink_tx(const u8 *frame, u32 length);
static u32 route_tx(const u8 *frame, u32 length);

static void set_link(NET_DEV *nd, u32 up)
{
    if (nd->st->link == up)
        return;
    nd->st->link = up;
    if (up)
        nd->up_order = ++up_counter;
    if (nd->claim) {
        nd->claim->link(nd->claim->context, up);
        return;
    }
    choose();
}

void net_frame_in(NET_DEV *nd, const u8 *frame, u32 length)
{
    if (length < ETH_HLEN || length > ETH_FRAME_MAX + 4) {
        nd->st->rx_short++;
        return;
    }
    nd->st->rx_frames++;
    net_add64(&nd->st->rx_bytes, &nd->st->rx_bytes_hi, length);
    /* A device that never sends a link notification still delivers frames; one
     * arriving is as good a sign of link as any. */
    if (!nd->st->link && !nd->link_down_seen)
        set_link(nd, 1);
    if (nd->claim) {
        nd->claim->rx(nd->claim->context, frame, length);
        return;
    }
    if (uplink() != nd) {
        if (second_rx(nd, frame, length))
            return;
        nd->st->rx_not_uplink++;
        return;
    }
    second_uplink_rx(frame, length);
    net_up(frame, length);
}

/* A frame for xnet, by whichever card it came. */
void net_up(const u8 *frame, u32 length)
{
    net_rx_note(frame, length);
    if (ipstack_rx(frame, length))
        return;                             /* the own stack's (make stack=1): never xnet's */
    if (net_win_extra_kb && window_rx(frame, length))
        return;                             /* beyond xnet's window: kept until it has room */
    if (net_rx) {
        PROF_START(t);
        net_rx(frame, length);
        PROF_END(rx_up, t, length);
    }
}

void net_link_report(NET_DEV *nd, u32 up)
{
    nd->stalled_ticks = 0;              /* the driver's own word replaces the stall watch's */
    bench_event(1, (u32)(nd - net_devs), up, (u32)__builtin_return_address(0));
    if (!up)
        nd->link_down_seen = 1;
    set_link(nd, up);
}

void net_speed_report(NET_DEV *nd, u32 mbps)
{
    if (nd->st->speed_mbps == mbps)
        return;
    nd->st->speed_mbps = mbps;
    choose();
}

void net_tx_done(NET_DEV *nd)
{
    if (nd->claim) {
        if (nd->claim->tx_idle)
            nd->claim->tx_idle(nd->claim->context);
        return;
    }
    second_tx_done(nd);
    if (uplink() == nd)
        ipstack_tx_done();
    if (net_tx_idle && uplink() == nd)
        net_tx_idle();
}

void net_claim(NET_DEV *nd, const NET_CLAIM *claim)
{
    nd->claim = claim;
    mem_sync();
    choose();
}

u32 net_link_up(void)
{
    NET_DEV *nd = uplink();

    return nd && nd->st->ready && nd->st->link;
}

u32 net_speed_mbps(void)
{
    NET_DEV *nd = uplink();

    return nd ? nd->st->speed_mbps : 0;
}

/* --- DMA memory ---------------------------------------------------------------- */

static u8 *scratch;
static u32 scratch_used;

/* MmAllocatePhysicalMemory(2, size, 4), as CUsbEnet: physically contiguous, so a
 * block larger than a page is still one DMA buffer. */
void *net_dma_scratch(u32 bytes)
{
    u8 *block;

    if (!scratch) {
        scratch = (u8 *)MmAllocatePhysicalMemory(2, SCRATCH_BYTES, 4);
        if (!scratch)
            return 0;
    }
    scratch_used = (scratch_used + 63u) & ~63u;
    if (scratch_used + bytes > SCRATCH_BYTES)
        return 0;
    block = scratch + scratch_used;
    scratch_used += bytes;
    return block;
}

/* A slot's block stays with the slot: the next device in it, of whatever driver,
 * carves it again, and it is taken anew only when that one wants more. It is what the
 * device asks for and no more: sized for the largest driver (659 KB) it took 467 KB of
 * the console's system memory an RTL8156B never used, and that memory runs out
 * (window.h). */
u32 net_dma_begin(NET_DEV *nd, u32 total)
{
    if (nd->rings && total > nd->rings_size) {
        MmFreePhysicalMemory(2, nd->rings);     /* the next device in the slot wants more */
        nd->rings = 0;
    }
    if (!nd->rings) {
        nd->rings = (u8 *)MmAllocatePhysicalMemory(2, total, 4);
        if (!nd->rings)
            return 0;
        nd->rings_size = total;
    }
    nd->rings_used = 0;
    return total <= nd->rings_size;
}

void *net_dma_alloc(NET_DEV *nd, u32 bytes)
{
    u8 *block;

    nd->rings_used = (nd->rings_used + 2047u) & ~2047u;
    if (!nd->rings || nd->rings_used + bytes > nd->rings_size)
        return 0;
    block = nd->rings + nd->rings_used;
    nd->rings_used += bytes;
    return block;
}

/* --- a device's lifetime ----------------------------------------------------- */

static u32 net_start(NET_DEV *nd)
{
    u32 step;

    nd->st->ready = 0;
    nd->st->link = 0;
    nd->st->tx_inflight = 0;
    nd->link_down_seen = 0;
    nd->claim = 0;
    if (!nd->drv)
        return 0xFFFFFFFFu;

    step = nd->drv->start(nd);
    nd->st->step = step;
    if (step)
        return step;
    mem_sync();
    nd->st->ready = 1;
    /* A device may report its link during start (an RNDIS phone answers
     * MEDIA_CONNECT_STATUS there), before it was ready: choose again now. */
    choose();
    return 0;
}

/* From RemoveDevice, at DISPATCH_LEVEL. */
static void net_detached(USB_DEV *dev)
{
    NET_DEV *nd = (NET_DEV *)dev->client;

    if (!nd)
        return;
    nd->st->ready = 0;
    if (console_mac_owner == nd)
        console_mac_owner = 0;          /* the next card to join takes it */
    mem_sync();
    if (nd->drv)
        nd->drv->detached(nd);
    nd->st->speed_mbps = 0;
    set_link(nd, 0);
    choose();
}

u32 net_dev_tx(NET_DEV *nd, const u8 *frame, u32 length)
{
    if (!nd->drv || !nd->st->ready || !nd->usb->active || length < ETH_HLEN ||
        length > ETH_FRAME_MAX) {
        nd->st->tx_drops++;
        return 0;
    }
    return nd->drv->xmit(nd, frame, length);
}

u32 net_dev_tx_space(NET_DEV *nd)
{
    return nd->drv && nd->st->ready && nd->drv->tx_space(nd);
}

/* The uplink's own transmit, for a held acknowledgement (acks.h) as for a frame. */
static u32 uplink_tx(const u8 *frame, u32 length)
{
    NET_DEV *nd = uplink();

    if (!nd || !nd->st->ready || !nd->usb->active || length < ETH_HLEN ||
        length > ETH_FRAME_MAX) {
        if (nd)
            nd->st->tx_drops++;
        return 0;
    }
    return nd->drv->xmit(nd, frame, length);
}

#if XE_STACK
/* The own stack's frames, as xnet's go: acknowledgements thinned (acks.h), and a frame of a
 * connection that came in by the second card goes back by it (second.h). */
u32 net_stack_tx(const u8 *frame, u32 length)
{
    if ((net_ack_thin > 1 || net_ack_holding) && acks_tx(frame, length, route_tx))
        return 1;
    return route_tx(frame, length);         /* by the second way when its connection came by it */
}
#endif

/* By the second way if the frame's connection came in by it (second.h), else the uplink. */
static u32 route_tx(const u8 *frame, u32 length)
{
    u32 sent;

    if (second_tx(frame, length, &sent))
        return sent;
    return uplink_tx(frame, length);
}

u32 net_tx(const u8 *frame, u32 length)
{
    if (bench_tx_refused())
        return 0;
    if (ipstack_takes_tx())
        return 1;                           /* stack=1: the own stack is the console's, not xnet */
    /* thinned before the second way rewrites the frame: a held acknowledgement is xnet's
     * own, and takes its way out when it goes */
    if (net_win_extra_kb) {
        /* the window xnet states is made larger in its own frame, and put back once the
         * card (or the held acknowledgement) has its copy: xnet may send the frame again */
        WIN_PATCH patch;
        u32 sent;

        window_tx((u8 *)frame, length, &patch);
        sent = ((net_ack_thin > 1 || net_ack_holding) && acks_tx(frame, length, route_tx)) ||
               route_tx(frame, length);
        if (patch.n)
            win_restore((u8 *)frame, &patch);
        return sent;
    }
    if ((net_ack_thin > 1 || net_ack_holding) && acks_tx(frame, length, route_tx))
        return 1;
    return route_tx(frame, length);
}

void net_rx_note(const u8 *frame, u32 length)
{
    if (net_ack_thin > 1)
        acks_rx(frame, length);
    if (net_ack_holding)
        acks_flush(route_tx, 0);
}

u32 net_tx_space(void)
{
    NET_DEV *nd = uplink();

    return nd && nd->st->ready && nd->drv->tx_space(nd);
}

/* --- who consumes the frames ------------------------------------------------- */

volatile NET_STATE net_state;

static u32 any_ready(void)
{
    u32 i;

    for (i = 0; i < NET_MAX; i++)
        if (net_devs[i].drv && net_devs[i].drv->kind != XE_KIND_AUDIO && net_stats[i].ready)
            return 1;
    return 0;
}

static void no_frames(const u8 *frame, u32 length)
{
    (void)frame;
    (void)length;
}

/* Switch who consumes frames. xnet needs a card that is up; until it is, the mode
 * is remembered in xe_config and applied by the next bring-up. A mode only the
 * bench has (standalone) is its receiver, or drop in a build without it. */
static u32 set_mode(u32 mode)
{
    BENCH_RX receiver;

    xe_config.mode = mode;
    if (mode == XE_MODE_XNET) {
        if (!any_ready()) {
            net_rx = no_frames;
            net_state.mode = XE_MODE_DROP;
            return STATUS_UNSUCCESSFUL;
        }
        net_state.xnet_status = xnet_enable();
        if (net_state.xnet_status) {
            net_rx = no_frames;
            net_state.mode = XE_MODE_DROP;
            return net_state.xnet_status;
        }
        net_state.mode = XE_MODE_XNET;
        return 0;
    }

    xnet_disable();
    net_link_cb = 0;
    receiver = bench_mode(mode);
    net_rx = receiver ? (NET_RX)receiver : no_frames;
    net_state.mode = receiver ? mode : XE_MODE_DROP;
    return 0;
}

void net_layer_init(void)
{
    xnet_title_pool();                  /* before a title's xnet starts */
    file_setting("xedrivers.ini", "uplink", uplink_wanted, sizeof(uplink_wanted));
    second_init();
    acks_init();
    window_init();
    ipstack_init();
    set_mode(xe_config.mode == XE_MODE_XNET ? XE_MODE_DROP : xe_config.mode);
    xe_config.mode = XE_MODE;
}

void net_bring_up(USB_DEV *dev)
{
    NET_DEV *nd = (NET_DEV *)dev->client;

    if (!nd)
        return;
    net_state.start_count++;
    net_state.start_result = net_start(nd);
    if (net_state.start_result || nd->drv->kind == XE_KIND_AUDIO)
        return;
    set_mode(xe_config.mode);
}

void net_tick(void)
{
    u32 i;

    if (net_ack_holding)
        acks_flush(route_tx, 1);        /* nothing came in to send them by: at most a tick late */
    if (net_win_extra_kb)
        window_tick();
    if (net_switching && ++switch_ticks >= SWITCH_GAP_TICKS) {
        net_switching = 0;
        mem_sync();
        choose();
    }

    for (i = 0; i < NET_MAX; i++) {
        NET_DEV *nd = &net_devs[i];

        if (nd->drv && nd->drv->tick && nd->st->ready) {
            u32 t0 = net_tb(), took;

            nd->drv->tick(nd);
            took = net_tb() - t0;
            if (took > net_tick_longest[i])
                net_tick_longest[i] = took;     /* xectl.py worker: per slot */
        }
    }
    owner_watch();
    /* A link the stall watch took down while the card itself kept it comes back after
     * STALL_RETRY_TICKS: a card that refused frames for a while (2026-11-10, bench_tx_refuse)
     * otherwise stayed off until the next boot. Refusing still, it is taken down again. */
    for (i = 0; i < NET_MAX; i++) {
        NET_DEV *nd = &net_devs[i];

        if (!nd->stalled_ticks)
            continue;
        if (!nd->drv || !nd->st->ready) {
            nd->stalled_ticks = 0;
        } else if (++nd->stalled_ticks >= STALL_RETRY_TICKS) {
            nd->stalled_ticks = 0;
            net_stall_retries++;
            set_link(nd, 1);
        }
    }
    if (xnet_tick_stalled() && uplink()) {
        NET_DEV *nd = uplink();

        bench_event(3, (u32)(nd - net_devs), xnet_stats.xq_stall_depth, 0);
        net_link_report(nd, 0);
        nd->stalled_ticks = 1;
    }
}

/* A driver's own command goes to the uplink's driver if it answers it, else to the
 * first device whose driver does. */
static u32 driver_command(u32 what, volatile u32 *args)
{
    NET_DEV *nd = uplink();
    u32 i;

    if (nd && nd->drv->command && nd->drv->command(nd, what, args))
        return 1;
    for (i = 0; i < NET_MAX; i++) {
        nd = &net_devs[i];
        if (nd->drv && nd->st->ready && nd->drv->command &&
            nd->drv->command(nd, what, args))
            return 1;
    }
    return 0;
}

u32 net_command(u32 what, volatile u32 *args)
{
    switch (what) {
    case CMD_MODE:
        args[5] = set_mode(args[1]);
        return 1;
    case CMD_XNET_IFACE:
        xnet_rx_iface = args[1];
        args[5] = 0;
        return 1;
    default:
        return driver_command(what, args);
    }
}

/* --- which driver gets a device ------------------------------------------- */

/* The kernel asks per interface of the current configuration; AddDevice and the
 * scan of what enumerated before we loaded know only the device. So a driver that
 * took the device by an interface is remembered with the device's VID:PID, and
 * asked for again when only the device is known. */
static const XE_DRIVER *offered;
static u32 offered_id;
static const XE_DRIVER *pending;

static u32 product(const u8 *device)
{
    return (u32)(device[8] | device[9] << 8) << 16 | (u32)(device[10] |
                                                           device[11] << 8);
}

static u32 client_match(const u8 *interface, const u8 *device)
{
    const XE_DRIVER *best = 0;
    u32 i, score, top = XE_MATCH_NONE;

    if (!device)
        return 0;
    for (i = 0; i < xe_driver_count; i++) {
        score = xe_drivers[i]->match(device, interface);
        if (score > top) {
            top = score;
            best = xe_drivers[i];
        }
    }
    if (best && interface) {
        offered = best;
        offered_id = product(device);
    }
    if (!best && !interface && offered && product(device) == offered_id)
        best = offered;
    if (!best)
        return 0;
    pending = best;
    return 1;
}

/* Inside AddDevice: the slot's NET_DEV takes the driver just matched. */
static void client_attached(USB_DEV *dev)
{
    NET_DEV *nd = &net_devs[dev->index];
    u32 i;

    nd->usb = dev;
    nd->st = &net_stats[dev->index];
    nd->priv = 0;
    nd->drv = pending;
    nd->st->where = dev->where;
    for (i = 0; i < xe_driver_count; i++)
        if (xe_drivers[i] == pending)
            nd->st->driver = i + 1;
    dev->client = nd;
}

static const USB_CLIENT client = { client_match, client_attached, net_detached };

void net_register(void)
{
    u32 i;

    for (i = 0; i < NET_MAX; i++)
        net_devs[i].st = &net_stats[i];
    usb_register(&client);
}
