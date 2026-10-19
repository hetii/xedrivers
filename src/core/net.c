/* The network device; see net.h. Everything here is the same for every card: which
 * driver has the device, its DMA memory, frames handed up, link state, counters.
 * How frames reach the wire is the driver's (src/drivers/).
 */

#include "net.h"
#include "usb.h"
#include "driver.h"
#include "prof.h"

#define SCRATCH_BYTES 4096

volatile NET_RX net_rx;
volatile NET_LINK net_link_cb;
volatile NET_TX_IDLE net_tx_idle;
u8 net_mac[ETH_ALEN];
volatile NET_STATS net_stats;
const XE_DRIVER *volatile xe_driver;

#if XE_PROF
volatile NET_PROF net_prof;

/* Read off ordinal 131 on 17559; the frequency fits the low word. */
extern u32 KeQueryPerformanceFrequency(void);

void prof_init(void)
{
    net_prof.freq = KeQueryPerformanceFrequency();
    net_prof.enabled = 1;
}
#endif

static u32 link_down_seen;

void net_add64(volatile u32 *lo, volatile u32 *hi, u32 n)
{
    u32 v = *lo + n;

    if (v < n)
        (*hi)++;
    *lo = v;
}

/* --- frames and link, from the driver --------------------------------------- */

static void set_link(u32 up)
{
    if (net_stats.link == up)
        return;
    net_stats.link = up;
    if (net_link_cb)
        net_link_cb(up);
}

void net_frame_in(const u8 *frame, u32 length)
{
    if (length < ETH_HLEN || length > ETH_FRAME_MAX + 4) {
        net_stats.rx_short++;
        return;
    }
    net_stats.rx_frames++;
    net_add64(&net_stats.rx_bytes, &net_stats.rx_bytes_hi, length);
    /* A device that never sends a link notification still delivers frames; one
     * arriving is as good a sign of link as any. */
    if (!net_stats.link && !link_down_seen)
        set_link(1);
    if (net_rx) {
        PROF_START(t);
        net_rx(frame, length);
        PROF_END(rx_up, t, length);
    }
}

void net_link_report(u32 up)
{
    if (!up)
        link_down_seen = 1;
    set_link(up);
}

void net_speed_report(u32 mbps)
{
    net_stats.speed_mbps = mbps;
}

void net_tx_done(void)
{
    if (net_tx_idle)
        net_tx_idle();
}

u32 net_link_up(void)
{
    return net_stats.ready && net_stats.link;
}

u32 net_speed_mbps(void)
{
    return net_stats.speed_mbps;
}

/* --- DMA memory ---------------------------------------------------------------- */

static u8 *scratch;
static u32 scratch_used;
static u8 *rings;
static u32 rings_size;
static u32 rings_used;

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

u32 net_dma_begin(u32 total)
{
    if (!rings) {
        rings = (u8 *)MmAllocatePhysicalMemory(2, total, 4);
        if (!rings)
            return 0;
        rings_size = total;
    }
    rings_used = 0;
    return total <= rings_size;
}

void *net_dma_alloc(u32 bytes)
{
    u8 *block;

    rings_used = (rings_used + 2047u) & ~2047u;
    if (!rings || rings_used + bytes > rings_size)
        return 0;
    block = rings + rings_used;
    rings_used += bytes;
    return block;
}

/* --- the device's lifetime ---------------------------------------------------- */

u32 net_start(void)
{
    const XE_DRIVER *drv = xe_driver;
    u32 step;

    net_stats.ready = 0;
    net_stats.link = 0;
    net_stats.tx_inflight = 0;
    link_down_seen = 0;
    if (!drv)
        return 0xFFFFFFFFu;

    step = drv->start();
    net_stats.step = step;
    if (step)
        return step;
    mem_sync();
    net_stats.ready = 1;
    return 0;
}

/* From RemoveDevice, at DISPATCH_LEVEL. */
void net_detached(void)
{
    const XE_DRIVER *drv = xe_driver;

    net_stats.ready = 0;
    mem_sync();
    if (drv)
        drv->detached();
    set_link(0);
}

u32 net_tx(const u8 *frame, u32 length)
{
    const XE_DRIVER *drv = xe_driver;

    if (!drv || !net_stats.ready || !usb.active || length < ETH_HLEN ||
        length > ETH_FRAME_MAX) {
        net_stats.tx_drops++;
        return 0;
    }
    return drv->xmit(frame, length);
}

u32 net_tx_space(void)
{
    const XE_DRIVER *drv = xe_driver;

    return drv && net_stats.ready && drv->tx_space();
}

u32 net_command(u32 what, volatile u32 *args)
{
    const XE_DRIVER *drv = xe_driver;

    return drv && drv->command && drv->command(what, args);
}

/* --- which driver gets the device ------------------------------------------- */

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

static void client_attached(void)
{
    u32 i;

    xe_driver = pending;
    for (i = 0; i < xe_driver_count; i++)
        if (xe_drivers[i] == pending)
            net_stats.driver = i + 1;
}

static const USB_CLIENT client = { client_match, client_attached, net_detached };

void net_register(void)
{
    usb_register(&client);
}
