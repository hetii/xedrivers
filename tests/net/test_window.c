/* The receive window made larger: what is told to the peer, what is kept, and when it goes up. */

#include <stdio.h>
#include <string.h>
#include "window.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

#define PEER 0xC0A8019Du        /* 192.168.1.157 */
#define OURS 0xC0A801D2u

static u32 sum16(const u8 *p, u32 n, u32 c)
{
    u32 i;

    for (i = 0; i + 1 < n; i += 2)
        c += (u32)p[i] << 8 | p[i + 1];
    if (n & 1)
        c += (u32)p[n - 1] << 8;
    return c;
}

/* The TCP checksum of a frame as it stands: 0 when it is right. */
static u32 tcp_sum(const u8 *f)
{
    const u8 *ip = f + 14;
    u32 total = (u32)ip[2] << 8 | ip[3], n = total - 20, c;

    c = sum16(ip + 12, 8, 6 + n);
    c = sum16(ip + 20, n, c);
    while (c >> 16)
        c = (c & 0xFFFF) + (c >> 16);
    return ~c & 0xFFFF;
}

/* A TCP frame; `out`: from the console's port 2121 to the peer's 40000, else back.
 * `scale` < 0: no window scale option. */
static u32 tcp(u8 *f, int out, u32 seq, u32 ack, u32 flags, u32 window, u32 payload, int scale)
{
    u8 *ip = f + 14, *t = ip + 20;
    u32 doff = scale >= 0 ? 24 : 20, total = 20 + doff + payload, src = out ? OURS : PEER,
        dst = out ? PEER : OURS, sp = out ? 2121 : 40000, dp = out ? 40000 : 2121, c;

    memset(f, 0, 14 + total);
    memset(t + doff, 0x5A, payload);
    f[12] = 0x08;
    ip[0] = 0x45; ip[2] = (u8)(total >> 8); ip[3] = (u8)total; ip[9] = 6;
    ip[12] = (u8)(src >> 24); ip[13] = (u8)(src >> 16); ip[14] = (u8)(src >> 8); ip[15] = (u8)src;
    ip[16] = (u8)(dst >> 24); ip[17] = (u8)(dst >> 16); ip[18] = (u8)(dst >> 8); ip[19] = (u8)dst;
    t[0] = (u8)(sp >> 8); t[1] = (u8)sp; t[2] = (u8)(dp >> 8); t[3] = (u8)dp;
    t[4] = (u8)(seq >> 24); t[5] = (u8)(seq >> 16); t[6] = (u8)(seq >> 8); t[7] = (u8)seq;
    t[8] = (u8)(ack >> 24); t[9] = (u8)(ack >> 16); t[10] = (u8)(ack >> 8); t[11] = (u8)ack;
    t[12] = (u8)(doff / 4 << 4); t[13] = (u8)flags;
    t[14] = (u8)(window >> 8); t[15] = (u8)window;
    if (scale >= 0) { t[20] = 1; t[21] = 3; t[22] = 3; t[23] = (u8)scale; }
    c = tcp_sum(f);
    t[16] = (u8)(c >> 8); t[17] = (u8)c;
    return 14 + total;
}

static u32 window_of(const u8 *f) { return (u32)f[14 + 20 + 14] << 8 | f[14 + 20 + 15]; }

static WIN_TABLE t;
static u8 pool[WIN_FRAMES * WIN_FRAME];

int main(void)
{
    static u8 f[1600], before[1600];
    WIN_PATCH p;
    WIN_FLOW *flow;
    u32 n, slot, extra = 256u << 10, i;

    /* A connection whose opening was not seen is left alone. */
    win_reset(&t, pool);
    n = tcp(f, 1, 1000, 5000, 0x10, 0x8000, 0, -1);
    memcpy(before, f, n);
    CHECK(!win_out(&t, f, n, extra, 0, &p) && !p.n && !memcmp(f, before, n));
    n = tcp(f, 0, 5000 + 200000, 1000, 0x10, 0x100, 1000, -1);
    CHECK(win_in(&t, f, n, extra, 0) == 0);

    /* xnet answers a SYN with window scale 1: the peer is given WIN_SHIFT, the checksum
     * holds, and the frame is as it was once put back. */
    n = tcp(f, 1, 999, 5000, 0x12, 0xFFFF, 0, 1);
    memcpy(before, f, n);
    CHECK(!win_out(&t, f, n, extra, 0, &p) && p.n);
    CHECK(f[14 + 20 + 23] == WIN_SHIFT && tcp_sum(f) == 0 && t.opened == 1);
    win_restore(f, &p);
    CHECK(!memcmp(f, before, n));

    /* An acknowledgement with xnet's 128 KB (0xFFFF << 1): the peer sees that and 256 KB. */
    n = tcp(f, 1, 1000, 5000, 0x10, 0xFFFF, 0, -1);
    memcpy(before, f, n);
    CHECK(!win_out(&t, f, n, extra, 1, &p) && p.n && tcp_sum(f) == 0);
    CHECK(window_of(f) == ((0xFFFFu << 1) + extra) >> WIN_SHIFT);
    win_restore(f, &p);
    CHECK(!memcmp(f, before, n));

    /* What fits xnet's own window goes up; what lies beyond is kept, in order. */
    n = tcp(f, 0, 5000, 1000, 0x10, 0x100, 1400, -1);
    CHECK(win_in(&t, f, n, extra, 2) == 0);
    n = tcp(f, 0, 5000 + 131070, 1000, 0x10, 0x100, 1400, -1);
    CHECK(win_in(&t, f, n, extra, 2) == 1);
    n = tcp(f, 0, 5000 + 131070 + 1400, 1000, 0x10, 0x100, 1400, -1);
    CHECK(win_in(&t, f, n, extra, 2) == 1 && t.held_now == 2);
    /* an older one that fits is not queued behind them */
    n = tcp(f, 0, 5000 + 1400, 1000, 0x10, 0x100, 1400, -1);
    CHECK(win_in(&t, f, n, extra, 2) == 0);
    /* a pure acknowledgement from the peer always goes up */
    n = tcp(f, 0, 5000 + 200000, 1000, 0x10, 0x100, 0, -1);
    CHECK(win_in(&t, f, n, extra, 2) == 0);

    /* With two held, less extra is told: the edge the peer knows does not move back. */
    n = tcp(f, 1, 1000, 5000, 0x10, 0xFFFF, 0, -1);
    flow = win_out(&t, f, n, extra, 3, &p);
    CHECK(!flow && window_of(f) == ((0xFFFFu << 1) + extra) >> WIN_SHIFT);

    /* xnet acknowledges 2800 more: its edge moves, the first held one fits, then the second. */
    n = tcp(f, 1, 1000, 5000 + 2800, 0x10, 0xFFFF, 0, -1);
    flow = win_out(&t, f, n, extra, 4, &p);
    CHECK(flow != 0);
    if (flow) {
        slot = win_take(&t, flow, 0);
        CHECK(slot != WIN_NONE && t.end[slot] == 5000 + 131070 + 1400 && t.length[slot] == 14 + 40 + 1400);
        win_free(&t, slot);
        slot = win_take(&t, flow, 0);
        CHECK(slot != WIN_NONE && t.end[slot] == 5000 + 131070 + 2800);
        win_free(&t, slot);
        CHECK(win_take(&t, flow, 0) == WIN_NONE && t.held_now == 0 && flow->held_bytes == 0);
    }

    /* Held and the edge still for WIN_PROBE ticks: the first goes up anyway. */
    n = tcp(f, 0, 5000 + 2800 + 131070, 1000, 0x10, 0x100, 1400, -1);
    CHECK(win_in(&t, f, n, extra, 5) == 1);
    CHECK(!win_stale(&t, 5 + WIN_PROBE - 1));
    flow = win_stale(&t, 5 + WIN_PROBE);
    CHECK(flow && win_take(&t, flow, 0) == WIN_NONE);
    if (flow) {
        slot = win_take(&t, flow, 1);
        CHECK(slot != WIN_NONE);
        win_free(&t, slot);
    }

    /* More than extra and half again is not kept. */
    for (i = 0; i < 400; i++) {
        n = tcp(f, 0, 5000 + 2800 + 131070 + i * 1400, 1000, 0x10, 0x100, 1400, -1);
        if (win_in(&t, f, n, extra, 6) != 1)
            break;
    }
    CHECK(i == ((extra + extra / 2) / 1400 < WIN_FRAMES ? (extra + extra / 2) / 1400 : WIN_FRAMES) &&
          t.held_now == i);

    /* A reset lets all of it go, and the place with it. */
    n = tcp(f, 0, 5000, 1000, 0x14, 0, 0, -1);
    CHECK(win_in(&t, f, n, extra, 7) == 0 && t.held_now == 0 && t.dropped == i);
    n = tcp(f, 1, 1000, 5000, 0x10, 0x8000, 0, -1);
    CHECK(!win_out(&t, f, n, extra, 8, &p) && !p.n);

    /* No window scale offered (a 16 KB socket): the field itself says up to 64 KB. */
    n = tcp(f, 1, 999, 5000, 0x12, 0x4000, 0, -1);
    CHECK(!win_out(&t, f, n, extra, 9, &p) && !p.n);
    n = tcp(f, 1, 1000, 5000, 0x10, 0x4000, 0, -1);
    win_out(&t, f, n, extra, 9, &p);
    CHECK(window_of(f) == 0xFFFF && tcp_sum(f) == 0);
    n = tcp(f, 0, 5000 + 0x4000, 1000, 0x10, 0x100, 1400, -1);
    CHECK(win_in(&t, f, n, extra, 9) == 1);
    n = tcp(f, 0, 5000 + 0x4000 - 1400, 1000, 0x10, 0x100, 1400, -1);
    CHECK(win_in(&t, f, n, extra, 9) == 0);

    /* A SYN of xnet's own: the scale is replaced; an answer without one turns scaling off. */
    win_reset(&t, pool);
    n = tcp(f, 1, 999, 0, 0x02, 0xFFFF, 0, 1);
    win_out(&t, f, n, extra, 0, &p);
    CHECK(f[14 + 20 + 23] == WIN_SHIFT && tcp_sum(f) == 0);
    n = tcp(f, 0, 4999, 1000, 0x12, 0xFFFF, 0, -1);
    CHECK(win_in(&t, f, n, extra, 0) == 0);
    n = tcp(f, 1, 1000, 5000, 0x10, 0x4000, 0, -1);
    win_out(&t, f, n, extra, 1, &p);
    CHECK(window_of(f) == 0xFFFF);

    /* Closed both ways: the place is free at once for another connection. */
    win_reset(&t, pool);
    n = tcp(f, 1, 999, 5000, 0x12, 0xFFFF, 0, 1);
    win_out(&t, f, n, extra, 0, &p);
    n = tcp(f, 0, 5000, 1000, 0x11, 0x100, 0, -1);
    CHECK(win_in(&t, f, n, extra, 1) == 0);
    n = tcp(f, 1, 1000, 5001, 0x11, 0xFFFF, 0, -1);
    win_out(&t, f, n, extra, 1, &p);
    n = tcp(f, 1, 1001, 5001, 0x10, 0xFFFF, 0, -1);
    CHECK(!win_out(&t, f, n, extra, 1, &p) && !p.n);

    if (!fails)
        printf("all window tests passed\n");
    return fails != 0;
}
