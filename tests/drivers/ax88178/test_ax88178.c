/* Native tests for the AX88178 bulk framing: a stream of header + frame pieces as
 * asix_rx_fixup_internal (Linux) reads it, frames split across transfers, and the
 * transmit header as asix_tx_fixup writes it. Built and run on the host by
 * `make test`. */

#include <stdio.h>
#include <string.h>

#include "ax88178_frames.h"

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

static u32 got, got_len[8];
static u8 got_first[8], got_last[8];

static void on_frame(const u8 *frame, u32 length, void *context)
{
    (void)context;
    if (got < 8) {
        got_len[got] = length;
        got_first[got] = frame[0];
        got_last[got] = frame[length - 1];
    }
    got++;
}

static u32 put_frame(u8 *t, u32 len, u8 tag)
{
    u32 h = ((len ^ 0xFFFFu) << 16) | len;

    t[0] = (u8)h; t[1] = (u8)(h >> 8); t[2] = (u8)(h >> 16); t[3] = (u8)(h >> 24);
    memset(t + 4, tag, len);
    t[4 + len - 1] = (u8)(tag + 1);
    if (len & 1)
        t[4 + len] = 0;
    return 4 + len + (len & 1);
}

int main(void)
{
    static AX178_RX rx;
    u8 s[8192], f[1600], o[1700];
    u32 n = 0, cut, r;

    /* Three frames in one transfer, one of odd length. */
    ax178_rx_reset(&rx);
    n += put_frame(s + n, 60, 0x10);
    n += put_frame(s + n, 1513, 0x20);
    n += put_frame(s + n, 98, 0x30);
    r = ax178_rx_feed(&rx, s, n, on_frame, 0);
    CHECK(r == 3 && got == 3);
    CHECK(got_len[0] == 60 && got_len[1] == 1513 && got_len[2] == 98);
    CHECK(got_first[1] == 0x20 && got_last[1] == 0x21);

    /* The same stream cut inside the second frame (at an even offset, as the chip
     * pads): it is finished from the next transfer. */
    got = 0;
    ax178_rx_reset(&rx);
    cut = 64 + 4 + 700;
    r = ax178_rx_feed(&rx, s, cut, on_frame, 0);
    CHECK(r == 1 && got == 1);
    r = ax178_rx_feed(&rx, s + cut, n - cut, on_frame, 0);
    CHECK(r == 2 && got == 3 && got_len[1] == 1513 && got_last[1] == 0x21);

    /* Cut inside a header: its second half comes first in the next transfer. */
    got = 0;
    ax178_rx_reset(&rx);
    cut = 64 + 2;
    r = ax178_rx_feed(&rx, s, cut, on_frame, 0);
    CHECK(r == 1);
    r = ax178_rx_feed(&rx, s + cut, n - cut, on_frame, 0);
    CHECK(r == 2 && got == 3);

    /* A header that does not check: dropped, counted, the next transfer starts
     * clean. */
    got = 0;
    ax178_rx_reset(&rx);
    s[0] ^= 0x01;
    r = ax178_rx_feed(&rx, s, n, on_frame, 0);
    CHECK(r == 0 && rx.resyncs == 1);
    s[0] ^= 0x01;
    r = ax178_rx_feed(&rx, s, n, on_frame, 0);
    CHECK(r == 3);

    /* Transmit: the header, and the padding word on a packet boundary. */
    memset(f, 0x77, sizeof(f));
    n = ax178_tx_build(o, sizeof(o), f, 1514, 512);
    CHECK(n == 1518 && o[0] == 0xEA && o[1] == 0x05 && o[2] == 0x15 && o[3] == 0xFA);
    n = ax178_tx_build(o, sizeof(o), f, 1020, 512);       /* 4 + 1020 = 1024 */
    CHECK(n == 1028 && o[1024] == 0 && o[1025] == 0 && o[1026] == 0xFF && o[1027] == 0xFF);
    CHECK(ax178_tx_build(o, 100, f, 1514, 512) == 0);

    if (failures)
        return 1;
    printf("all ax88178 tests passed\n");
    return 0;
}
