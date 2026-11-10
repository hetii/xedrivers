/* See second.h. The first half is pure byte code (tests/net/test_second.c); the second is
 * the plugin's, built with multipath=1 only. */

#include "second.h"

/* Nothing of this file in a plugin built without multipath=1; the host tests take the
 * pure half always. */
#if XE_MULTIPATH || !defined(__powerpc__)

#define ETHERTYPE_IP  0x0800u
#define ETHERTYPE_ARP 0x0806u
#define PROTO_ICMP    1u
#define PROTO_TCP     6u
#define PROTO_UDP     17u

static u32 rd16(const u8 *p) { return (u32)p[0] << 8 | p[1]; }
static u32 rd32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static void wr16(u8 *p, u32 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static void wr32(u8 *p, u32 v) { wr16(p, v >> 16); wr16(p + 2, v); }

/* RFC 1624: HC' = ~(~HC + ~m + m'), in ones' complement, 16 bits at a time. */
void second_fix(u8 *sum, u32 was, u32 is)
{
    u32 s = (~rd16(sum) & 0xFFFFu) + (~was >> 16 & 0xFFFFu) + (~was & 0xFFFFu) +
            (is >> 16) + (is & 0xFFFFu);

    while (s >> 16)
        s = (s & 0xFFFFu) + (s >> 16);
    wr16(sum, ~s);
}

u32 second_key(const u8 *frame, u32 length, u32 in, u32 *peer_ip, u32 *ports, u32 *proto)
{
    const u8 *ip = frame + ETH_HLEN, *l4;
    u32 ihl, p;

    if (length < ETH_HLEN + 20 || rd16(frame + 12) != ETHERTYPE_IP || (ip[0] >> 4) != 4)
        return 0;
    ihl = (ip[0] & 0x0Fu) * 4;
    if (ihl < 20 || length < ETH_HLEN + ihl + 8 || (rd16(ip + 6) & 0x1FFFu))
        return 0;                               /* a later fragment has no ports */
    p = ip[9];
    l4 = ip + ihl;
    *proto = p;
    *peer_ip = rd32(ip + (in ? 12 : 16));
    if (p == PROTO_TCP || p == PROTO_UDP)
        *ports = in ? rd16(l4) << 16 | rd16(l4 + 2) : rd16(l4 + 2) << 16 | rd16(l4);
    else if (p == PROTO_ICMP)
        *ports = 0;                             /* a peer's echoes: one flow */
    else
        return 0;
    return 1;
}

static u32 bucket(u32 peer_ip, u32 ports, u32 proto)
{
    u32 h = peer_ip ^ ports * 0x9E3779B1u ^ proto;

    return (h ^ h >> 15) & (SECOND_FLOWS - 1);
}

static u32 same(const SECOND_FLOW *f, u32 peer_ip, u32 ports, u32 proto)
{
    return f->proto == proto && f->peer_ip == peer_ip && f->ports == ports;
}

/* Two places a flow can be. A new one takes the free one, else that of the flow unused
 * longer: thrown out, a flow is learned again by its next frame in. */
void second_learn(SECOND_TABLE *t, u32 peer_ip, u32 ports, u32 proto, const u8 *peer_mac,
                  u32 now)
{
    u32 b = bucket(peer_ip, ports, proto), i;
    SECOND_FLOW *f = &t->flow[b], *g = &t->flow[(b + 1) & (SECOND_FLOWS - 1)];

    if (!same(f, peer_ip, ports, proto)) {
        if (same(g, peer_ip, ports, proto))
            f = g;
        else if (f->proto && (!g->proto || now - g->seen > now - f->seen))
            f = g;
    }
    if (!same(f, peer_ip, ports, proto)) {
        f->proto = 0;                           /* not half of one and half of another */
        f->peer_ip = peer_ip;
        f->ports = ports;
        t->learned++;
    }
    for (i = 0; i < ETH_ALEN; i++)
        f->peer_mac[i] = peer_mac[i];
    f->seen = now;
    f->proto = proto;
}

static SECOND_FLOW *place(SECOND_TABLE *t, u32 peer_ip, u32 ports, u32 proto)
{
    u32 b = bucket(peer_ip, ports, proto);
    SECOND_FLOW *f = &t->flow[b], *g = &t->flow[(b + 1) & (SECOND_FLOWS - 1)];

    if (same(f, peer_ip, ports, proto))
        return f;
    return same(g, peer_ip, ports, proto) ? g : 0;
}

const SECOND_FLOW *second_find(SECOND_TABLE *t, u32 peer_ip, u32 ports, u32 proto, u32 now)
{
    SECOND_FLOW *f = place(t, peer_ip, ports, proto);

    if (!f)
        return 0;
    if (proto != PROTO_TCP && now - f->seen > SECOND_IDLE) {
        f->proto = 0;
        return 0;
    }
    f->seen = now;
    return f;
}

u32 second_forget(SECOND_TABLE *t, u32 peer_ip, u32 ports, u32 proto)
{
    SECOND_FLOW *f = place(t, peer_ip, ports, proto);

    if (!f)
        return 0;
    f->proto = 0;
    return 1;
}

u32 second_refused(SECOND_STUCK *s, u32 now, u32 hold, u32 quit)
{
    if (!s->refusing) {
        s->refusing = 1;
        s->since = now;
    }
    if (now - s->since >= quit)
        return SECOND_QUIT;
    return now - s->since >= hold ? SECOND_DROP : SECOND_WAIT;
}

void second_took(SECOND_STUCK *s)
{
    s->refusing = 0;
}

void second_rewrite(u8 *frame, u32 dest, u32 ip_new, const u8 *mac)
{
    u8 *ip = frame + ETH_HLEN, *field = ip + (dest ? 16 : 12), *l4;
    u32 ihl = (ip[0] & 0x0Fu) * 4, was = rd32(field), i;

    for (i = 0; i < ETH_ALEN; i++)
        frame[(dest ? 0 : ETH_ALEN) + i] = mac[i];
    if (was == ip_new)
        return;
    wr32(field, ip_new);
    second_fix(ip + 10, was, ip_new);
    if (rd16(ip + 6) & 0x1FFFu)
        return;                                 /* the first fragment carries the checksum */
    l4 = ip + ihl;
    if (ip[9] == PROTO_TCP)
        second_fix(l4 + 16, was, ip_new);
    else if (ip[9] == PROTO_UDP && rd16(l4 + 6))
        second_fix(l4 + 6, was, ip_new);        /* 0: the sender left it out */
}

#if XE_MULTIPATH

#include "file.h"
#include "usb.h"
#include "driver.h"

volatile SECOND_STATS second_stats;

static SECOND_TABLE table;
NET_DEV *volatile second_dev;

/* A card that stops taking frames -- its link there, its ring never emptied -- must not hold
 * xnet's queue, which is the uplink's as well: a frame it refuses goes back to the queue only
 * while that has lasted under 200 ms (a busy card takes the next within one), is dropped after,
 * as the wire would have lost it, and after 2 s the card is given up until its link has been
 * down. */
#define TB_PER_MS       49875u
#define HOLD_TICKS      (200u * TB_PER_MS)
#define QUIT_TICKS      (2000u * TB_PER_MS)

static SECOND_STUCK stuck;
static NET_DEV *volatile gave_up_on;

static u32 tb(void)
{
#if defined(__powerpc__)
    u32 t;

    __asm__ volatile("mftb %0" : "=r"(t));
    return t;
#else
    return 0;
#endif
}

/* The table's tick: 2^26 of the time base's, 1.35 s. */
static u32 table_now(void)
{
#if defined(__powerpc__)
    u32 hi, lo;

    __asm__ volatile("mftbu %0\n\tmftb %1" : "=&r"(hi), "=r"(lo));
    return hi << 6 | lo >> 26;
#else
    return 0;
#endif
}
static u8 arp_reply[ETH_HLEN + 28] __attribute__((aligned(4)));

/* "a.b.c.d", host order; 0 for anything else. */
static u32 parse_ip(const char *s)
{
    u32 ip = 0, part = 0, digits = 0, dots = 0;

    for (;; s++) {
        if (*s >= '0' && *s <= '9') {
            part = part * 10 + (u32)(*s - '0');
            if (++digits > 3 || part > 255)
                return 0;
        } else if ((*s == '.' || !*s) && digits) {
            ip = ip << 8 | part;
            part = digits = 0;
            if (!*s)
                return dots == 3 ? ip : 0;
            if (++dots > 3)
                return 0;
        } else {
            return 0;
        }
    }
}

void second_init(void)
{
    char value[20];

    if (file_setting("xedrivers.ini", "second_ip", value, sizeof(value)))
        second_stats.ip = parse_ip(value);
}

/* The address is this card's now: said to the network (a gratuitous ARP request), or what
 * was sent to the card before goes on to that one's Ethernet address until the peers ask
 * again -- with the first card's cable out, a transfer by the second address stood still
 * though another card had taken the address over (2026-11-11). */
static u32 announce_left, announced_at;

static void announce(NET_DEV *nd)
{
    static u8 frame[ETH_HLEN + 28] __attribute__((aligned(4)));
    u32 i;

    for (i = 0; i < ETH_ALEN; i++) {
        frame[i] = 0xFF;
        frame[ETH_ALEN + i] = nd->mac[i];
        frame[ETH_HLEN + 8 + i] = nd->mac[i];
        frame[ETH_HLEN + 18 + i] = 0;
    }
    wr16(frame + 12, ETHERTYPE_ARP);
    wr16(frame + ETH_HLEN, 1);                  /* Ethernet */
    wr16(frame + ETH_HLEN + 2, ETHERTYPE_IP);
    frame[ETH_HLEN + 4] = ETH_ALEN;
    frame[ETH_HLEN + 5] = 4;
    wr16(frame + ETH_HLEN + 6, 1);              /* a request, for our own address */
    wr32(frame + ETH_HLEN + 14, second_stats.ip);
    wr32(frame + ETH_HLEN + 24, second_stats.ip);
    if (net_dev_tx(nd, frame, sizeof(frame)))
        second_stats.announced++;
    announced_at = tb();
    if (announce_left)
        announce_left--;
}

/* The card: one with a link that is neither the uplink nor someone's, on the other host
 * controller if there is one (its own ceiling), else any; none without an uplink. */
void second_choose(void)
{
    u32 active = net_active, i;
    NET_DEV *best = 0;

    if (!second_stats.ip)
        return;
    if (gave_up_on && (!gave_up_on->st->ready || !gave_up_on->st->link))
        gave_up_on = 0;                 /* its link has been down: it may be tried again */
    for (i = 0; active && i < NET_MAX; i++) {
        NET_DEV *nd = &net_devs[i];

        if (i + 1 == active || !nd->drv || nd->drv->kind == XE_KIND_AUDIO || nd->claim ||
            !nd->st->ready || !nd->st->link || nd == gave_up_on)
            continue;
        if (!best || ((nd->st->where ^ net_devs[active - 1].st->where) & 3u) >
                     ((best->st->where ^ net_devs[active - 1].st->where) & 3u))
            best = nd;
    }
    /* The table is kept across a change: a connection whose card went and came back, or
     * whose address another card now answers for, goes on with its next frame in. */
    if (best != second_dev) {
        second_took(&stuck);
        if (best) {
            announce_left = 3;          /* now, and twice more a second apart (second_rx_slow) */
            announce(best);
        }
        second_dev = best;
        second_stats.slot = best ? (u32)(best - net_devs) + 1 : 0;
        mem_sync();
    }
}

static void arp_in(NET_DEV *nd, const u8 *frame, u32 length)
{
    const u8 *arp = frame + ETH_HLEN;
    u32 i;

    if (length < ETH_HLEN + 28 || rd16(arp) != 1 || rd16(arp + 2) != ETHERTYPE_IP ||
        rd16(arp + 6) != 1 || rd32(arp + 24) != second_stats.ip)
        return;
    for (i = 0; i < ETH_ALEN; i++) {
        arp_reply[i] = frame[ETH_ALEN + i];
        arp_reply[ETH_ALEN + i] = nd->mac[i];
        arp_reply[ETH_HLEN + 8 + i] = nd->mac[i];
    }
    wr16(arp_reply + 12, ETHERTYPE_ARP);
    for (i = 0; i < 6; i++)
        arp_reply[ETH_HLEN + i] = arp[i];               /* htype, ptype, hlen, plen */
    wr16(arp_reply + ETH_HLEN + 6, 2);
    wr32(arp_reply + ETH_HLEN + 14, second_stats.ip);
    for (i = 0; i < 10; i++)
        arp_reply[ETH_HLEN + 18 + i] = arp[8 + i];      /* their addresses */
    if (net_dev_tx(nd, arp_reply, sizeof(arp_reply)))
        second_stats.arp_replies++;
}

/* Any card that is not the uplink may bring what is for the address: a peer goes on sending
 * to the card that had it until it asks again (after the cable was back, two of four new
 * connections never opened: the PC still had the wireless card's address, which no longer
 * took them). Only the second way's card answers ARP and sends. */
u32 second_rx_slow(NET_DEV *nd, const u8 *frame, u32 length)
{
    u32 peer, ports, proto, mine = nd == second_dev;

    if (length < ETH_HLEN + 20)
        return mine;
    if (mine && announce_left && tb() - announced_at >= 1000u * TB_PER_MS)
        announce(nd);                   /* once more: the first may have met a port not yet up */
    if (rd16(frame + 12) == ETHERTYPE_ARP) {
        if (mine)
            arp_in(nd, frame, length);
        return mine;
    }
    if (rd16(frame + 12) != ETHERTYPE_IP || rd32(frame + ETH_HLEN + 16) != second_stats.ip ||
        !second_key(frame, length, 1, &peer, &ports, &proto)) {
        second_stats.not_ours += mine;
        return mine;
    }
    if (!second_stats.xnet_ip || !net_rx) {
        second_stats.no_xnet_ip++;
        return 1;
    }
    second_learn(&table, peer, ports, proto, frame + ETH_ALEN, table_now());
    second_stats.flows = table.learned;
    mem_sync();
    /* in place: the frame is the card's receive buffer, ours until we return */
    second_rewrite((u8 *)(unsigned long)frame, 1, second_stats.xnet_ip, net_console_mac);
    second_stats.in++;
    net_rx(frame, length);
    return 1;
}

u32 second_tx_slow(const u8 *frame, u32 length, u32 *sent)
{
    NET_DEV *nd = second_dev;
    const SECOND_FLOW *f;
    u32 peer, ports, proto, source;

    if (!nd || !second_key(frame, length, 0, &peer, &ports, &proto))
        return 0;
    source = rd32(frame + ETH_HLEN + 12);
    if (source != second_stats.ip) {
        if (source && source != second_stats.xnet_ip)
            second_stats.xnet_ip = source;
        f = second_find(&table, peer, ports, proto, table_now());
        if (!f)
            return 0;
        /* in place, in xnet's own packet: a retransmission of it comes with our address
         * already there, and is sent as it is (above) */
        second_rewrite((u8 *)(unsigned long)frame, 0, second_stats.ip, nd->mac);
        {
            u32 i;
            u8 *out = (u8 *)(unsigned long)frame;

            for (i = 0; i < ETH_ALEN; i++)
                out[i] = f->peer_mac[i];
        }
    }
    *sent = nd->st->ready && net_dev_tx_space(nd) ? net_dev_tx(nd, frame, length) : 0;
    if (*sent) {
        second_took(&stuck);
        second_stats.out++;
        return 1;
    }
    switch (second_refused(&stuck, tb(), HOLD_TICKS, QUIT_TICKS)) {
    case SECOND_WAIT:
        second_stats.out_refused++;
        return 1;
    case SECOND_QUIT:
        gave_up_on = nd;
        second_dev = 0;
        second_stats.slot = 0;
        second_stats.gave_up++;
        mem_sync();
        second_took(&stuck);
        /* fall through */
    default:
        second_stats.out_dropped++;
        *sent = 1;                      /* not xnet's to send again: TCP does that */
        return 1;
    }
}

/* A SYN without ACK from the uplink: the peer opens a connection to the console's own
 * address. If the table has those addresses and ports from an earlier one by the second
 * way, the answers would leave by the wrong card. */
void second_uplink_slow(const u8 *frame, u32 length)
{
    const u8 *ip = frame + ETH_HLEN;
    u32 peer, ports, proto;

    if (length < ETH_HLEN + 40 || rd16(frame + 12) != ETHERTYPE_IP || ip[9] != PROTO_TCP)
        return;
    if ((ip[(ip[0] & 0x0Fu) * 4 + 13] & 0x12u) != 0x02u)
        return;
    if (second_key(frame, length, 1, &peer, &ports, &proto) &&
        second_forget(&table, peer, ports, proto))
        second_stats.moved++;
}

#endif /* XE_MULTIPATH */

#endif /* XE_MULTIPATH || host */
