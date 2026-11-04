/* Native tests for the AX88179 / AX88178A bulk framing: receive transfers built
 * the way ax88179_rx_fixup (Linux) reads them, the transmit header as
 * ax88179_tx_fixup writes it. Built and run on the host by `make test`. */

#include <stdio.h>
#include <string.h>

#include "ax88179_frames.h"

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

static u32 got, got_len[8];
static u8 got_first[8];

static void on_frame(const u8 *frame, u32 length, void *context)
{
    (void)context;
    if (got < 8) {
        got_len[got] = length;
        got_first[got] = frame[0];
    }
    got++;
}

static void put32(u8 *p, u32 v)
{
    p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}

/* A transfer of frames of `lens` bytes (the 2-byte alignment included), each
 * followed by its metadata word and a dummy one, as current firmware sends. */
static u32 build(u8 *t, const u32 *lens, const u32 *flags, u32 n)
{
    u32 at = 0, i, meta, count = 0;

    for (i = 0; i < n; i++) {
        memset(t + at, 0, (lens[i] + 7) & ~7u);
        t[at + 2] = (u8)(0xA0 + i);             /* first byte of the Ethernet header */
        at += (lens[i] + 7) & ~7u;
    }
    meta = at;
    for (i = 0; i < n; i++) {
        put32(t + at, lens[i] << 16 | flags[i]);
        put32(t + at + 4, 0x80000000u);         /* the dummy word: length 0, drop */
        at += 8;
        count += 2;
    }
    at += 4;                                    /* padding2 */
    put32(t + at, count | meta << 16);
    return at + 4;
}

int main(void)
{
    u8 t[8192], h[8];
    u32 lens[3] = { 2 + 60, 2 + 1514, 2 + 98 }, flags[3] = { 0, 0, 0 };
    u32 bad = 0, n;
    s32 r;

    n = build(t, lens, flags, 3);
    r = ax179_rx_split(t, n, on_frame, 0, &bad);
    CHECK(r == 3 && bad == 0 && got == 3);
    CHECK(got_len[0] == 60 && got_len[1] == 1514 && got_len[2] == 98);
    CHECK(got_first[0] == 0xA0 && got_first[1] == 0xA1 && got_first[2] == 0xA2);

    /* A frame the chip marked with a CRC error is counted, not delivered. */
    got = 0;
    flags[1] = 0x20000000u;
    n = build(t, lens, flags, 3);
    r = ax179_rx_split(t, n, on_frame, 0, &bad);
    CHECK(r == 2 && bad == 1 && got == 2);

    /* A header pointing past the transfer does not parse. */
    put32(t + n - 4, 6u | (n + 16) << 16);
    CHECK(ax179_rx_split(t, n, on_frame, 0, &bad) < 0);
    /* Too short to hold the header. */
    CHECK(ax179_rx_split(t, 3, on_frame, 0, &bad) < 0);
    /* An empty transfer: nothing, no error. */
    put32(t, 0);
    CHECK(ax179_rx_split(t, 4, on_frame, 0, &bad) == 0);

    /* The transmit header: the length, and the padding flag only when frame and
     * header end on a 512-byte packet boundary. */
    ax179_tx_header(h, 1514, 512);
    CHECK(h[0] == 0xEA && h[1] == 0x05 && h[2] == 0 && h[3] == 0);
    CHECK(h[4] == 0 && h[5] == 0 && h[6] == 0 && h[7] == 0);
    ax179_tx_header(h, 1016, 512);              /* 1016 + 8 = 1024 */
    CHECK(h[4] == 0x00 && h[5] == 0x80 && h[6] == 0x00 && h[7] == 0x80);

    if (failures)
        return 1;
    printf("all ax88179 tests passed\n");
    return 0;
}
