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

#define CMD_MODE       5u   /* [1] XE_MODE_* */
#define CMD_XNET_IFACE 13u  /* [1] interface id given to xnet's receive callback */

#define SCRATCH_BYTES 16384

/* A slot's ring block, at least: the most any driver asks (rndis, 16 receive and
 * 16 transmit buffers of 16 and 24 KB), so whichever device comes next to the slot
 * fits -- an AIC8800 leaves and comes back as another product, a phone replaces a
 * card. */
#define NET_RINGS_MIN (16 * 16384 + 16 * 24576 + 2 * 2048)

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
 * address (tests/xectl.py cmd 18, src/bench/debug). Whatever comes back is only
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

/* 1 if `a` should be the uplink rather than `b` (both with a link up). */
static u32 better(const NET_DEV *a, const NET_DEV *b, const NET_DEV *current)
{
    if (a->drv->kind != b->drv->kind)
        return a->drv->kind > b->drv->kind;
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
static void choose(void)
{
    NET_DEV *current = uplink(), *best = 0;
    u32 i;

    if (net_switching)
        return;                 /* decided again when the gap ends (net_tick) */
    for (i = 0; i < NET_MAX; i++) {
        NET_DEV *nd = &net_devs[i];

        if (!nd->drv || !nd->st->ready || !nd->st->link)
            continue;
        if (!best || better(nd, best, current))
            best = nd;
    }
    if (best == current)
        return;
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

/* --- frames and link, from the drivers -------------------------------------- */

static void set_link(NET_DEV *nd, u32 up)
{
    if (nd->st->link == up)
        return;
    nd->st->link = up;
    if (up)
        nd->up_order = ++up_counter;
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
    if (uplink() != nd) {
        nd->st->rx_not_uplink++;
        return;
    }
    if (net_rx) {
        PROF_START(t);
        net_rx(frame, length);
        PROF_END(rx_up, t, length);
    }
}

void net_link_report(NET_DEV *nd, u32 up)
{
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
    if (net_tx_idle && uplink() == nd)
        net_tx_idle();
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
 * carves it again; NET_RINGS_MIN makes it big enough for any of them. */
u32 net_dma_begin(NET_DEV *nd, u32 total)
{
    if (!nd->rings) {
        u32 size = total > NET_RINGS_MIN ? total : NET_RINGS_MIN;

        nd->rings = (u8 *)MmAllocatePhysicalMemory(2, size, 4);
        if (!nd->rings)
            return 0;
        nd->rings_size = size;
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
    mem_sync();
    if (nd->drv)
        nd->drv->detached(nd);
    nd->st->speed_mbps = 0;
    nd->st->link = 0;
    choose();
}

u32 net_tx(const u8 *frame, u32 length)
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
        if (net_devs[i].drv && net_stats[i].ready)
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
    if (net_state.start_result)
        return;
    set_mode(xe_config.mode);
}

void net_tick(void)
{
    u32 i;

    if (net_switching && ++switch_ticks >= SWITCH_GAP_TICKS) {
        net_switching = 0;
        mem_sync();
        choose();
    }

    for (i = 0; i < NET_MAX; i++) {
        NET_DEV *nd = &net_devs[i];

        if (nd->drv && nd->drv->tick && nd->st->ready)
            nd->drv->tick(nd);
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
