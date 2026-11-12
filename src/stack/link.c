/* The stack's network interface. See ipstack.h.
 *
 * Every frame the uplink (or the second way, second.c) brings is the stack's once it is up.
 * In: the cards deliver at DISPATCH_LEVEL on the USB processor, and lwIP is thread-level
 * code. So a frame that is the stack's is copied into a ring there and a thread hands
 * the ring to lwIP (tcpip_input, which with core locking does the work in that thread).
 * Out: lwIP's frames go into a queue the uplink is fed from -- by the thread that queued
 * one, and by the card's transmit completion when it had no room.
 */
#include "lwip/opt.h"
#include "lwip/tcpip.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/pbuf.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/dhcp.h"
#include "lwip/dns.h"
#include "lwip/stats.h"
#include "lwip/memp.h"
#include "netif/ethernet.h"
#include <string.h>

#include "ipstack.h"
#include "file.h"

#if XE_STACK

#define RX_RING     128u                /* frames, a power of two */
#define TX_QUEUE    128u
#define SLOT        1536u

volatile IPSTACK_STATS ipstack_stats;
volatile u32 ipstack_ready;
volatile u32 ipstack_servers;

static struct netif nif;
static KEVENT wake;

static u8 rx_frame[RX_RING][SLOT] __attribute__((aligned(4)));
static u16 rx_length[RX_RING];
static volatile u32 rx_in, rx_out;
static volatile u32 rx_idle;            /* the thread waits: a frame put in wakes it */
static KSPIN_LOCK rx_lock;              /* two cards may bring frames at once */

static u8 tx_frame[TX_QUEUE][SLOT] __attribute__((aligned(4)));
static u16 tx_length[TX_QUEUE];
static volatile u32 tx_in, tx_out, tx_busy, tx_starved;

static u32 have_mac;                    /* nif.hwaddr is the uplink's, and DHCP runs */

void ipstack_assert(const char *what)
{
    ipstack_stats.asserts++;
    ipstack_stats.assert_text = what;
}

unsigned int ipstack_rand(void)
{
    static u32 seed;
    u32 t;

    __asm__ volatile("mftb %0" : "=r"(t));
    seed ^= t;
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    return seed;
}

/* --- out ---------------------------------------------------------------------- */

/* One at a time feeds the card; whoever finds it busy leaves the frames to it. */
static void drain(void)
{
    for (;;) {
        u32 full = 0;

        if (__sync_lock_test_and_set(&tx_busy, 1))
            return;
        while (tx_out != tx_in) {
            u32 at = tx_out & (TX_QUEUE - 1);

            if (!net_stack_tx(tx_frame[at], tx_length[at])) {
                full = 1;               /* the card's completion calls again */
                break;
            }
            tx_out = tx_out + 1;
            ipstack_stats.tx++;
        }
        __sync_lock_release(&tx_busy);
        mem_sync();
        if (full || tx_out == tx_in)
            return;                     /* else one came while this ran, and found it busy */
    }
}

/* lwIP, under its core lock: so one thread at a time puts frames in. */
static err_t link_out(struct netif *netif, struct pbuf *p)
{
    u32 at, waiting = tx_in - tx_out;

    (void)netif;
    if (p->tot_len > ETH_FRAME_MAX)
        return ERR_BUF;
    if (waiting >= TX_QUEUE) {
        ipstack_stats.tx_dropped++;
        tx_starved = 1;                 /* told to send again when there is room */
        return ERR_MEM;
    }
    at = tx_in & (TX_QUEUE - 1);
    pbuf_copy_partial(p, tx_frame[at], p->tot_len, 0);
    tx_length[at] = p->tot_len;
    mem_sync();
    tx_in = tx_in + 1;
    if (waiting + 1 > ipstack_stats.tx_most)
        ipstack_stats.tx_most = waiting + 1;
    drain();
    return ERR_OK;
}

void ipstack_tx_done(void)
{
    if (!ipstack_ready)
        return;
    if (tx_out != tx_in)
        drain();
    if (tx_starved && tx_in - tx_out < TX_QUEUE / 2)
        KeSetEvent(&wake, 0, 0);
}

/* --- in ----------------------------------------------------------------------- */

static void put(const u8 *frame, u32 length)
{
    u32 irql, at, i, waiting;
    u8 *to;

    if (length > SLOT)
        return;
    irql = KfAcquireSpinLock(&rx_lock);
    waiting = rx_in - rx_out;
    if (waiting >= RX_RING) {
        KfReleaseSpinLock(&rx_lock, irql);
        ipstack_stats.rx_dropped++;
        return;
    }
    at = rx_in & (RX_RING - 1);
    to = rx_frame[at];
    for (i = 0; i + 4 <= length; i += 4)
        *(u32 *)(to + i) = *(const u32 *)(frame + i);
    for (; i < length; i++)
        to[i] = frame[i];
    rx_length[at] = (u16)length;
    rx_in = rx_in + 1;
    if (waiting + 1 > ipstack_stats.rx_most)
        ipstack_stats.rx_most = waiting + 1;
    KfReleaseSpinLock(&rx_lock, irql);
    /* only when the thread sleeps: an event set for each of 18000 frames a second is
     * scheduler work on the processor that serves USB */
    if (rx_idle)
        KeSetEvent(&wake, 0, 0);
}

u32 ipstack_rx_slow(const u8 *frame, u32 length)
{
    put(frame, length);
    return 1;
}

/* The uplink's MAC is the stack's: DHCP on it, once and again when the uplink changes. */
static void watch_link(void)
{
    u8 mac[6];
    u32 link = net_uplink_mac(mac), i;

    LOCK_TCPIP_CORE();
    if (link && (!have_mac || memcmp(mac, nif.hwaddr, 6))) {
        if (have_mac)
            dhcp_release_and_stop(&nif);
        for (i = 0; i < 6; i++)
            nif.hwaddr[i] = mac[i];
        netif_set_link_up(&nif);
        dhcp_start(&nif);
        have_mac = 1;
        ipstack_stats.dhcp_starts++;
    } else if (link && !netif_is_link_up(&nif)) {
        netif_set_link_up(&nif);
        dhcp_network_changed_link_up(&nif);
    } else if (!link && have_mac && netif_is_link_up(&nif)) {
        netif_set_link_down(&nif);
    }
    ipstack_stats.ip = ip4_addr_get_u32(netif_ip4_addr(&nif));
    ipstack_stats.gateway = ip4_addr_get_u32(netif_ip4_gw(&nif));
    ipstack_stats.dns = ip4_addr_get_u32(ip_2_ip4(dns_getserver(0)));
    UNLOCK_TCPIP_CORE();
}

static void loop(void *arg)
{
    u32 ticks = 0;

    (void)arg;
    for (;;) {
        s64 interval = -100000;         /* 10 ms: a wake-up missed costs no more */

        rx_idle = 1;
        mem_sync();
        if (rx_out == rx_in)
            KeWaitForSingleObject(&wake, 0, 0, 0, &interval);
        rx_idle = 0;
        if (++ticks % 10 == 0 || !have_mac)
            watch_link();               /* every 100 ms or so */
        while (rx_out != rx_in) {
            u32 at = rx_out & (RX_RING - 1), length = rx_length[at];
            struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)length, PBUF_POOL);

            if (!p) {
                ipstack_stats.rx_no_pbuf++;
            } else {
                pbuf_take(p, rx_frame[at], (u16_t)length);
                if (nif.input(p, &nif) != ERR_OK) {
                    pbuf_free(p);
                    ipstack_stats.rx_refused++;
                }
                ipstack_stats.rx++;
            }
            rx_out = rx_out + 1;
        }
        ipstack_stats.pbufs_used = lwip_stats.memp[MEMP_PBUF_POOL]->used;
        ipstack_stats.pbufs_most = lwip_stats.memp[MEMP_PBUF_POOL]->max;
        ipstack_stats.pbufs_refused = lwip_stats.memp[MEMP_PBUF_POOL]->err;
        if (tx_out != tx_in)
            drain();
        if (tx_starved && tx_in - tx_out < TX_QUEUE / 2) {
            struct tcp_pcb *pcb;

            tx_starved = 0;
            ipstack_stats.tx_starved++;
            LOCK_TCPIP_CORE();
            for (pcb = tcp_active_pcbs; pcb; pcb = pcb->next)
                tcp_output(pcb);
            UNLOCK_TCPIP_CORE();
        }
    }
}

static err_t nif_init(struct netif *netif)
{
    netif->name[0] = 'x';
    netif->name[1] = 'e';
    netif->output = etharp_output;
    netif->linkoutput = link_out;
    netif->mtu = 1500;
    netif->hwaddr_len = 6;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    return ERR_OK;
}

/* lwIP's own thread, once it runs. */
static void started(void *arg)
{
    ip4_addr_t none;

    (void)arg;
    ip4_addr_set_zero(&none);
    netif_add(&nif, &none, &none, &none, 0, nif_init, tcpip_input);
    netif_set_default(&nif);
    netif_set_up(&nif);
    sys_thread_new("link", loop, 0, 0x8000, 20);
    ipstack_stats.up = 1;
    mem_sync();
    ipstack_ready = 1;
}

void ipstack_init(void)
{
    char value[4];

    if (file_setting("xedrivers.ini", "stack_servers", value, sizeof(value)))
        ipstack_servers = value[0] == '1';
    KeInitializeEvent(&wake, 1, 0);
    tcpip_init(started, 0);
}

#endif /* XE_STACK */
