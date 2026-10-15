/* The standalone responder; see stack.h. Byte-wise loads and stores throughout so
 * the same file runs in tests/ on a little-endian host. Received frames arrive at
 * DISPATCH_LEVEL from the receive ring, one at a time on the USB processor, which
 * is why one static reply buffer is enough. */

#include "stack.h"
#include "net.h"
#include "usb.h"

#define ETHERTYPE_IP  0x0800
#define ETHERTYPE_ARP 0x0806
#define PROTO_ICMP    1
#define PROTO_UDP     17

volatile u32 stack_ip = 0x0A240002u;           /* 10.36.0.2 */
volatile STACK_STATS stack_stats;

static u8 reply[ETH_FRAME_MAX];
static u8 peer_mac[ETH_ALEN];

static u32 rd16(const u8 *p)
{
    return ((u32)p[0] << 8) | p[1];
}

static u32 rd32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static void wr16(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 8);
    p[1] = (u8)v;
}

static void wr32(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
}

static void copy(u8 *dst, const u8 *src, u32 n)
{
    u32 i;

    for (i = 0; i < n; i++)
        dst[i] = src[i];
}

u16 ip_checksum(const u8 *data, u32 length, u32 initial)
{
    u32 sum = initial, i;

    for (i = 0; i + 1 < length; i += 2)
        sum += rd16(data + i);
    if (length & 1)
        sum += (u32)data[length - 1] << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)~sum;
}

static void learn_peer(const u8 *mac, u32 ip)
{
    copy(peer_mac, mac, ETH_ALEN);
    stack_stats.peer_ip = ip;
    stack_stats.peer_mac_hi = rd16(mac);
    stack_stats.peer_mac_lo = rd32(mac + 2);
}

/* Ethernet header of a reply: back to whoever sent `frame`, from us. */
static void eth_reply(const u8 *frame, u32 type)
{
    copy(reply, frame + 6, ETH_ALEN);
    copy(reply + 6, net_mac, ETH_ALEN);
    wr16(reply + 12, type);
}

static void arp_input(const u8 *frame, u32 length)
{
    const u8 *arp = frame + ETH_HLEN;

    if (length < ETH_HLEN + 28 || rd16(arp) != 1 || rd16(arp + 2) != ETHERTYPE_IP)
        return;
    if (rd32(arp + 24) != stack_ip)
        return;

    learn_peer(arp + 8, rd32(arp + 14));
    if (rd16(arp + 6) != 1)                     /* only requests are answered */
        return;

    eth_reply(frame, ETHERTYPE_ARP);
    copy(reply + ETH_HLEN, arp, 6);             /* htype, ptype, hlen, plen */
    wr16(reply + ETH_HLEN + 6, 2);              /* reply */
    copy(reply + ETH_HLEN + 8, net_mac, ETH_ALEN);
    wr32(reply + ETH_HLEN + 14, stack_ip);
    copy(reply + ETH_HLEN + 18, arp + 8, 10);   /* their sha and spa */
    if (net_tx(reply, ETH_HLEN + 28))
        stack_stats.arp_replies++;
}

/* An IP reply with the request's payload swapped round: same protocol, addresses
 * exchanged, fresh header checksum. `l4` is the transport header and payload. */
static u32 ip_reply(const u8 *frame, u32 ihl, u32 total)
{
    const u8 *ip = frame + ETH_HLEN;
    u8 *out = reply + ETH_HLEN;

    eth_reply(frame, ETHERTYPE_IP);
    out[0] = 0x45;
    out[1] = 0;
    wr16(out + 2, total - ihl + 20);
    wr16(out + 4, rd16(ip + 4));
    wr16(out + 6, 0);
    out[8] = 64;
    out[9] = ip[9];
    wr16(out + 10, 0);
    wr32(out + 12, stack_ip);
    copy(out + 16, ip + 12, 4);
    wr16(out + 10, ip_checksum(out, 20, 0));
    copy(out + 20, ip + ihl, total - ihl);
    return ETH_HLEN + 20 + total - ihl;
}

static void ip_input(const u8 *frame, u32 length)
{
    const u8 *ip = frame + ETH_HLEN;
    u32 ihl, total, n;

    if (length < ETH_HLEN + 20 || (ip[0] >> 4) != 4)
        return;
    ihl = (u32)(ip[0] & 0x0F) * 4;
    total = rd16(ip + 2);
    if (ihl < 20 || total < ihl || ETH_HLEN + total > length)
        return;
    if (rd32(ip + 16) != stack_ip) {
        stack_stats.not_ours++;
        return;
    }
    if (rd16(ip + 6) & 0x3FFF)                  /* fragments: not handled */
        return;

    learn_peer(frame + 6, rd32(ip + 12));

    if (ip[9] == PROTO_ICMP && total - ihl >= 8 && ip[ihl] == 8) {
        n = ip_reply(frame, ihl, total);
        reply[ETH_HLEN + 20] = 0;               /* echo reply */
        wr16(reply + ETH_HLEN + 22, 0);
        wr16(reply + ETH_HLEN + 22,
             ip_checksum(reply + ETH_HLEN + 20, total - ihl, 0));
        if (net_tx(reply, n))
            stack_stats.icmp_replies++;
        return;
    }

    if (ip[9] == PROTO_UDP && total - ihl >= 8) {
        const u8 *udp = ip + ihl;
        u32 port = rd16(udp + 2);

        if (port == 9) {
            u32 before = stack_stats.discard_bytes;

            stack_stats.discard_frames++;
            stack_stats.discard_bytes = before + (total - ihl - 8);
            if (stack_stats.discard_bytes < before)
                stack_stats.discard_bytes_hi++;
            return;
        }
        if (port == 7) {
            n = ip_reply(frame, ihl, total);
            wr16(reply + ETH_HLEN + 20, port);          /* swap the ports */
            wr16(reply + ETH_HLEN + 22, rd16(udp));
            wr16(reply + ETH_HLEN + 26, 0);             /* no UDP checksum */
            if (net_tx(reply, n))
                stack_stats.udp_echo++;
        }
    }
}

void stack_input(const u8 *frame, u32 length)
{
    u32 type = rd16(frame + 12);

    if (type == ETHERTYPE_ARP)
        arp_input(frame, length);
    else if (type == ETHERTYPE_IP)
        ip_input(frame, length);
}

/* The generator. One frame is built and only the IP id and checksum change, so
 * the cost per datagram is the copy into the transmit slot. When the ring is full
 * it gives the processor up for a millisecond rather than spinning. */
static u8 blast_frame[ETH_FRAME_MAX];

u32 stack_blast(u32 count, u32 payload, u32 port)
{
    u8 *ip = blast_frame + ETH_HLEN;
    u8 *udp = ip + 20;
    u32 sent = 0, i, total;

    if (payload > ETH_FRAME_MAX - ETH_HLEN - 28)
        payload = ETH_FRAME_MAX - ETH_HLEN - 28;
    if (!stack_stats.peer_ip)
        return 0;
    total = 20 + 8 + payload;

    copy(blast_frame, peer_mac, ETH_ALEN);
    copy(blast_frame + 6, net_mac, ETH_ALEN);
    wr16(blast_frame + 12, ETHERTYPE_IP);
    ip[0] = 0x45;
    ip[1] = 0;
    wr16(ip + 2, total);
    wr16(ip + 6, 0x4000);                       /* don't fragment */
    ip[8] = 64;
    ip[9] = PROTO_UDP;
    wr32(ip + 12, stack_ip);
    wr32(ip + 16, stack_stats.peer_ip);
    wr16(udp, 5009);
    wr16(udp + 2, port);
    wr16(udp + 4, 8 + payload);
    wr16(udp + 6, 0);
    for (i = 0; i < payload; i++)
        udp[8 + i] = (u8)i;

    stack_stats.blast_busy = 1;
    while (sent < count && net_link_up()) {
        wr32(udp + 8, sent);                    /* sequence number, for loss */
        wr16(ip + 4, sent);
        wr16(ip + 10, 0);
        wr16(ip + 10, ip_checksum(ip, 20, 0));
        if (net_tx(blast_frame, ETH_HLEN + total)) {
            sent++;
            stack_stats.blast_sent = sent;
        } else {
            usb_sleep(1);
        }
    }
    stack_stats.blast_busy = 0;
    return sent;
}
