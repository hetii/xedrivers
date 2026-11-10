/* See second.h. The first half is pure byte code (tests/net/test_second.c); the second is
 * the plugin's, built with multipath=1 only. */

#include "second.h"

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

/* Two places a flow can be; a new one takes the free one, else the first (the oldest there
 * is not known and need not be: a flow thrown out is learned again by its next frame in). */
void second_learn(SECOND_TABLE *t, u32 peer_ip, u32 ports, u32 proto, const u8 *peer_mac)
{
    u32 b = bucket(peer_ip, ports, proto), i;
    SECOND_FLOW *f = &t->flow[b], *g = &t->flow[(b + 1) & (SECOND_FLOWS - 1)];

    if (!same(f, peer_ip, ports, proto)) {
        if (same(g, peer_ip, ports, proto) || (f->proto && !g->proto))
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
    f->proto = proto;
}

const SECOND_FLOW *second_find(const SECOND_TABLE *t, u32 peer_ip, u32 ports, u32 proto)
{
    u32 b = bucket(peer_ip, ports, proto);
    const SECOND_FLOW *f = &t->flow[b], *g = &t->flow[(b + 1) & (SECOND_FLOWS - 1)];

    if (same(f, peer_ip, ports, proto))
        return f;
    return same(g, peer_ip, ports, proto) ? g : 0;
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
static NET_DEV *volatile dev;
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

/* The card: one with a link that is neither the uplink nor someone's, on the other host
 * controller if there is one (its own ceiling), else any. The worker's. */
void second_tick(void)
{
    u32 active = net_active, i;
    NET_DEV *best = 0;

    if (!second_stats.ip)
        return;
    for (i = 0; active && i < NET_MAX; i++) {
        NET_DEV *nd = &net_devs[i];

        if (i + 1 == active || !nd->drv || nd->drv->kind == XE_KIND_AUDIO || nd->claim ||
            !nd->st->ready || !nd->st->link)
            continue;
        if (!best || ((nd->st->where ^ net_devs[active - 1].st->where) & 3u) >
                     ((best->st->where ^ net_devs[active - 1].st->where) & 3u))
            best = nd;
    }
    if (best != dev) {
        dev = best;
        second_stats.slot = best ? (u32)(best - net_devs) + 1 : 0;
        mem_sync();
    }
}

u32 second_is(const NET_DEV *nd)
{
    return nd == dev;
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

u32 second_rx(NET_DEV *nd, const u8 *frame, u32 length)
{
    u32 peer, ports, proto;

    if (nd != dev || !second_stats.ip || length < ETH_HLEN + 20)
        return 0;
    if (rd16(frame + 12) == ETHERTYPE_ARP) {
        arp_in(nd, frame, length);
        return 1;
    }
    if (rd16(frame + 12) != ETHERTYPE_IP || rd32(frame + ETH_HLEN + 16) != second_stats.ip ||
        !second_key(frame, length, 1, &peer, &ports, &proto)) {
        second_stats.not_ours++;
        return 1;
    }
    if (!second_stats.xnet_ip || !net_rx) {
        second_stats.no_xnet_ip++;
        return 1;
    }
    second_learn(&table, peer, ports, proto, frame + ETH_ALEN);
    second_stats.flows = table.learned;
    mem_sync();
    /* in place: the frame is the card's receive buffer, ours until we return */
    second_rewrite((u8 *)(unsigned long)frame, 1, second_stats.xnet_ip, net_console_mac);
    second_stats.in++;
    net_rx(frame, length);
    return 1;
}

u32 second_tx(const u8 *frame, u32 length, u32 *sent)
{
    NET_DEV *nd = dev;
    const SECOND_FLOW *f;
    u32 peer, ports, proto, source;

    if (!nd || !second_stats.ip || !second_key(frame, length, 0, &peer, &ports, &proto))
        return 0;
    source = rd32(frame + ETH_HLEN + 12);
    if (source != second_stats.ip) {
        if (source && source != second_stats.xnet_ip)
            second_stats.xnet_ip = source;
        f = second_find(&table, peer, ports, proto);
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
    if (*sent)
        second_stats.out++;
    else
        second_stats.out_refused++;
    return 1;
}

#endif /* XE_MULTIPATH */
