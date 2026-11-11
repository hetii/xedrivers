/* Acknowledgement thinning: what a frame is, and which are kept back. */

#include <stdio.h>
#include <string.h>
#include "acks.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* A TCP frame from 192.168.1.210:`sport` to `dst`:40000. */
static u32 tcp(u8 *f, u32 dst, u32 sport, u32 number, u32 flags, u32 payload, u32 sack)
{
    u8 *ip = f + 14, *t = ip + 20;
    u32 doff = sack ? 32 : 20, total = 20 + doff + payload;

    memset(f, 0, 14 + total);
    f[12] = 0x08;
    ip[0] = 0x45; ip[2] = (u8)(total >> 8); ip[3] = (u8)total; ip[9] = 6;
    ip[12] = 192; ip[13] = 168; ip[14] = 1; ip[15] = 210;
    ip[16] = (u8)(dst >> 24); ip[17] = (u8)(dst >> 16); ip[18] = (u8)(dst >> 8); ip[19] = (u8)dst;
    t[0] = (u8)(sport >> 8); t[1] = (u8)sport; t[2] = 0x9C; t[3] = 0x40;
    t[8] = (u8)(number >> 24); t[9] = (u8)(number >> 16); t[10] = (u8)(number >> 8); t[11] = (u8)number;
    t[12] = (u8)(doff / 4 << 4); t[13] = (u8)flags;
    if (sack) { t[20] = 1; t[21] = 1; t[22] = 5; t[23] = 10; }
    return 14 + total;
}

int main(void)
{
    static u8 f[200];
    static ACK_TABLE t;
    u32 P = 0xC0A8019D, peer, ports, number, n, i, kept, sent;
    ACK_FLOW *flush, *due;

    n = tcp(f, P, 5202, 1000, 0x10, 0, 0);
    CHECK(ack_kind(f, n, &peer, &ports, &number) == 2 && peer == P && ports == (5202u << 16 | 40000) && number == 1000);
    n = tcp(f, P, 5202, 1000, 0x10, 100, 0); CHECK(ack_kind(f, n, &peer, &ports, &number) == 1);  /* data */
    n = tcp(f, P, 5202, 1000, 0x11, 0, 0);   CHECK(ack_kind(f, n, &peer, &ports, &number) == 1);  /* FIN */
    n = tcp(f, P, 5202, 1000, 0x12, 0, 0);   CHECK(ack_kind(f, n, &peer, &ports, &number) == 1);  /* SYN */
    n = tcp(f, P, 5202, 1000, 0x10, 0, 1);   CHECK(ack_kind(f, n, &peer, &ports, &number) == 1);  /* SACK */
    n = tcp(f, P, 5202, 1000, 0x10, 0, 0); f[14 + 9] = 17; CHECK(ack_kind(f, n, &peer, &ports, &number) == 0);
    n = tcp(f, P, 5202, 1000, 0x10, 0, 0); f[14 + 7] = 1;  CHECK(ack_kind(f, n, &peer, &ports, &number) == 0);

    /* one in four: the first at once, then three kept and the fourth sent */
    for (i = 0, kept = sent = 0; i < 40; i++) {
        n = tcp(f, P, 5202, 2000 + 2896 * i, 0x10, 0, 0);
        if (ack_out(&t, f, n, 4, 100 + i, 1000, &flush)) kept++; else sent++;
        CHECK(!flush);
    }
    CHECK(sent == 10 && kept == 30);
    /* off: none kept, and what was held is let go */
    n = tcp(f, P, 5202, 899000, 0x10, 0, 0); CHECK(!ack_out(&t, f, n, 4, 199, 1000, &flush));  /* the fourth */
    n = tcp(f, P, 5202, 900000, 0x10, 0, 0); CHECK(ack_out(&t, f, n, 4, 200, 1000, &flush));
    CHECK(t.held_now == 1);
    n = tcp(f, P, 5202, 900100, 0x10, 0, 0); CHECK(!ack_out(&t, f, n, 0, 201, 1000, &flush) && !t.held_now);
    /* a duplicate is never kept, and takes the held one's place */
    n = tcp(f, P, 5202, 900200, 0x10, 0, 0); CHECK(ack_out(&t, f, n, 4, 202, 1000, &flush));
    CHECK(!ack_out(&t, f, n, 4, 203, 1000, &flush) && !t.held_now);
    CHECK(!ack_out(&t, f, n, 4, 204, 1000, &flush));
    /* data of the connection takes the held one's place */
    n = tcp(f, P, 5202, 900300, 0x10, 0, 0); CHECK(ack_out(&t, f, n, 4, 205, 1000, &flush) && t.held_now == 1);
    n = tcp(f, P, 5202, 900300, 0x18, 50, 0); CHECK(!ack_out(&t, f, n, 4, 206, 1000, &flush) && !t.held_now);
    /* the hold: nothing sent for longer than it, and the next goes at once */
    n = tcp(f, P, 5202, 900400, 0x10, 0, 0); CHECK(ack_out(&t, f, n, 4, 207, 1000, &flush));
    n = tcp(f, P, 5202, 900500, 0x10, 0, 0); CHECK(!ack_out(&t, f, n, 4, 2000, 1000, &flush) && !t.held_now);
    /* a held one that waits is due after the hold, or at once when forced */
    n = tcp(f, P, 5202, 900600, 0x10, 0, 0); CHECK(ack_out(&t, f, n, 4, 2001, 1000, &flush));
    CHECK(!ack_due(&t, 2500, 1000, 0));
    due = ack_due(&t, 2500, 1000, 1); CHECK(due && due->held == n && !memcmp(due->frame, f, n));
    due = ack_due(&t, 3100, 1000, 0); CHECK(due != 0);
    ack_sent(&t, due, 3100); CHECK(!t.held_now && !ack_due(&t, 9999, 1000, 1));
    /* another connection that falls on a place holding one: that one has to go first */
    memset(&t, 0, sizeof(t));
    for (i = 0; i < 64; i++) {
        n = tcp(f, P + i, 5202, 5000, 0x10, 0, 0); ack_out(&t, f, n, 4, 10, 1000, &flush);   /* each one's first: sent */
        n = tcp(f, P + i, 5202, 6000, 0x10, 0, 0); kept = ack_out(&t, f, n, 4, 11, 1000, &flush);
        if (flush) {
            CHECK(!kept && flush->held);
            ack_sent(&t, flush, 11);
            CHECK(!ack_out(&t, f, n, 4, 11, 1000, &flush) && !flush);       /* a new place: its first goes */
        }
    }
    CHECK(t.held_now <= ACK_FLOWS);

    printf(fails ? "%d acks tests FAILED\n" : "all acks tests passed\n", fails);
    return fails != 0;
}
