/* The second way's pure half: checksum fixes, flow keys, the table, the rewrite. */

#include <stdio.h>
#include <string.h>
#include "second.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static u32 sum16(const u8 *d, u32 n, u32 s)
{
    u32 i;

    for (i = 0; i + 1 < n; i += 2)
        s += (u32)d[i] << 8 | d[i + 1];
    if (n & 1)
        s += (u32)d[n - 1] << 8;
    while (s >> 16)
        s = (s & 0xFFFF) + (s >> 16);
    return s;
}

/* 0 when the IP header's and the TCP/UDP checksum are right. */
static u32 wrong(const u8 *f, u32 length)
{
    const u8 *ip = f + 14, *l4 = ip + 20;
    u32 l4len = length - 34, pseudo;

    if (sum16(ip, 20, 0) != 0xFFFF)
        return 1;
    pseudo = sum16(ip + 12, 8, ip[9] + l4len);
    if (ip[9] == 17 && !(l4[6] | l4[7]))
        return 0;
    return sum16(l4, l4len, pseudo) != 0xFFFF ? 2 : 0;
}

static u32 frame(u8 *f, u32 proto, u32 src, u32 dst, u32 sport, u32 dport, u32 payload)
{
    u8 *ip = f + 14, *l4 = ip + 20;
    u32 hl = proto == 6 ? 20 : 8, l4len = hl + payload, i, s;

    memset(f, 0, 14 + 20 + l4len);
    memcpy(f, "\x02\x00\x00\x00\x00\x01\x02\x00\x00\x00\x00\x02\x08\x00", 14);
    ip[0] = 0x45; ip[2] = (u8)((20 + l4len) >> 8); ip[3] = (u8)(20 + l4len); ip[8] = 64; ip[9] = (u8)proto;
    for (i = 0; i < 4; i++) {
        ip[12 + i] = (u8)(src >> (24 - 8 * i));
        ip[16 + i] = (u8)(dst >> (24 - 8 * i));
    }
    s = ~sum16(ip, 20, 0) & 0xFFFF; ip[10] = (u8)(s >> 8); ip[11] = (u8)s;
    l4[0] = (u8)(sport >> 8); l4[1] = (u8)sport; l4[2] = (u8)(dport >> 8); l4[3] = (u8)dport;
    if (proto == 17) { l4[4] = (u8)(l4len >> 8); l4[5] = (u8)l4len; }
    else l4[12] = 0x50;
    for (i = 0; i < payload; i++)
        l4[hl + i] = (u8)(i * 7 + 3);
    s = ~sum16(l4, l4len, sum16(ip + 12, 8, proto + l4len)) & 0xFFFF;
    if (proto == 17 && !s) s = 0xFFFF;
    l4[proto == 6 ? 16 : 6] = (u8)(s >> 8); l4[proto == 6 ? 17 : 7] = (u8)s;
    return 14 + 20 + l4len;
}

int main(void)
{
    static u8 f[1514];
    static SECOND_TABLE t;
    static const u8 mac[6] = { 0x00, 0x25, 0xAE, 0xBE, 0x73, 0xC0 };
    static const u8 peer_mac[6] = { 2, 0, 0, 0, 0, 2 };
    u32 A = 0xC0A801D2, B = 0xC0A801D3, P = 0xC0A8019D, n, peer, ports, proto, i, protos[2] = { 6, 17 };
    const SECOND_FLOW *fl;

    for (i = 0; i < 2; i++) {
        u32 payload;

        for (payload = 0; payload < 40; payload += 13) {
            /* in: the peer to our second address, becomes the console's */
            n = frame(f, protos[i], P, B, 40000, 5202, payload);
            CHECK(!wrong(f, n));
            CHECK(second_key(f, n, 1, &peer, &ports, &proto));
            CHECK(peer == P && ports == (40000u << 16 | 5202) && proto == protos[i]);
            second_rewrite(f, 1, A, mac);
            CHECK(!wrong(f, n));
            CHECK(!memcmp(f, mac, 6) && f[14 + 16 + 3] == 0xD2);
            second_rewrite(f, 1, A, mac);               /* twice changes nothing */
            CHECK(!wrong(f, n));
            /* out: the console's answer, becomes the second address's */
            n = frame(f, protos[i], A, P, 5202, 40000, payload);
            CHECK(second_key(f, n, 0, &peer, &ports, &proto));
            CHECK(peer == P && ports == (40000u << 16 | 5202) && proto == protos[i]);
            second_rewrite(f, 0, B, mac);
            CHECK(!wrong(f, n));
            CHECK(!memcmp(f + 6, mac, 6) && f[14 + 12 + 3] == 0xD3);
        }
    }
    /* UDP without a checksum stays without */
    n = frame(f, 17, P, B, 1, 2, 8);
    f[14 + 20 + 6] = f[14 + 20 + 7] = 0;
    second_rewrite(f, 1, A, mac);
    CHECK(!(f[14 + 20 + 6] | f[14 + 20 + 7]) && !wrong(f, n));
    /* not IPv4, a later fragment, a protocol without ports */
    n = frame(f, 6, P, B, 1, 2, 0);
    f[12] = 0x86; CHECK(!second_key(f, n, 1, &peer, &ports, &proto)); f[12] = 0x08;
    f[14 + 7] = 5; CHECK(!second_key(f, n, 1, &peer, &ports, &proto)); f[14 + 7] = 0;
    f[14 + 9] = 47; CHECK(!second_key(f, n, 1, &peer, &ports, &proto));
    f[14 + 9] = 1; CHECK(second_key(f, n, 1, &peer, &ports, &proto) && ports == 0 && proto == 1);

    /* the table */
    CHECK(!second_find(&t, P, 1, 6, 0));
    second_learn(&t, P, 1, 6, peer_mac, 0);
    fl = second_find(&t, P, 1, 6, 0);
    CHECK(fl && !memcmp(fl->peer_mac, peer_mac, 6) && t.learned == 1);
    second_learn(&t, P, 1, 6, mac, 1);                  /* again: the way back follows */
    fl = second_find(&t, P, 1, 6, 1);
    CHECK(fl && !memcmp(fl->peer_mac, mac, 6) && t.learned == 1);
    CHECK(!second_find(&t, P, 1, 17, 1) && !second_find(&t, P, 2, 6, 1) &&
          !second_find(&t, P + 1, 1, 6, 1));
    /* a TCP connection may be idle for ever; anything else is forgotten */
    CHECK(second_find(&t, P, 1, 6, 1000000));
    second_learn(&t, P, 7, 17, peer_mac, 10);
    CHECK(second_find(&t, P, 7, 17, 10 + SECOND_IDLE));
    CHECK(!second_find(&t, P, 7, 17, 10 + 2 * SECOND_IDLE + 1));
    CHECK(!second_find(&t, P, 7, 17, 11));              /* and stays forgotten */
    /* forgotten on demand */
    CHECK(second_forget(&t, P, 1, 6) && !second_find(&t, P, 1, 6, 1) && !second_forget(&t, P, 1, 6));
    /* far more than it holds: the newest are there, and the one in use all along is kept */
    memset(&t, 0, sizeof(t));
    second_learn(&t, P, 4242, 6, peer_mac, 0);
    for (i = 0; i < 1000; i++) {
        second_learn(&t, P + 1 + i, i, 6, peer_mac, 1 + i);
        CHECK(second_find(&t, P, 4242, 6, 1 + i));      /* used now: never the older of two */
    }
    CHECK(second_find(&t, P + 1000, 999, 6, 1001));
    for (i = 0, n = 0; i < 1000; i++)
        n += second_find(&t, P + 1 + i, i, 6, 1001) != 0;
    CHECK(n > SECOND_FLOWS / 2 && n < SECOND_FLOWS);

    /* a card that takes nothing */
    {
        SECOND_STUCK st = { 0, 0 };

        CHECK(second_refused(&st, 0xFFFFFF00u, 100, 1000) == SECOND_WAIT);  /* the clock wraps */
        CHECK(second_refused(&st, 0xFFFFFF00u + 99, 100, 1000) == SECOND_WAIT);
        CHECK(second_refused(&st, 0xFFFFFF00u + 100, 100, 1000) == SECOND_DROP);
        CHECK(second_refused(&st, 0xFFFFFF00u + 999, 100, 1000) == SECOND_DROP);
        CHECK(second_refused(&st, 0xFFFFFF00u + 1000, 100, 1000) == SECOND_QUIT);
        second_took(&st);                                   /* one frame taken: from the start */
        CHECK(second_refused(&st, 5000, 100, 1000) == SECOND_WAIT);
        CHECK(second_refused(&st, 5050, 100, 1000) == SECOND_WAIT);
        second_took(&st);
        CHECK(second_refused(&st, 5200, 100, 1000) == SECOND_WAIT);
    }

    printf(fails ? "%d second tests FAILED\n" : "all second tests passed\n", fails);
    return fails != 0;
}
