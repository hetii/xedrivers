/* See wireless.h: a link's rate from what it was set up with, as 802.11 defines it --
 * data subcarriers x coded bits per subcarrier x streams / symbol time. The figure an
 * access point shows for a station (`iw station dump`) for the highest MCS the two agreed. */

#include "wireless.h"

/* Coded bits per subcarrier, times six, for MCS 0-9 (BPSK 1/2 ... 256-QAM 5/6). */
static const u8 bits6[10] = { 3, 6, 9, 12, 18, 24, 27, 30, 36, 40 };

/* Data subcarriers at 20, 40, 80 and 160 MHz (HT and VHT). */
static const u16 subcarriers[4] = { 52, 108, 234, 468 };

u32 wl_phy_mbps(u32 bw, u32 streams, u32 mcs, u32 sgi)
{
    u32 symbol = sgi ? 36 : 40;         /* tenths of a microsecond */

    if (bw > 3 || !streams || streams > 8 || mcs > 9)
        return 0;
    return (u32)subcarriers[bw] * bits6[mcs] * streams * 10 / (6 * symbol);
}

u32 wl_vht_best(u32 ours, u32 theirs, u32 *streams)
{
    u32 n, best = 0, mcs = 0;

    for (n = 0; n < 8; n++) {
        u32 a = ours >> (2 * n) & 3, b = theirs >> (2 * n) & 3, m;

        if (a == 3 || b == 3)
            break;
        m = (a < b ? a : b) + 7;        /* 0: MCS 0-7, 1: 0-8, 2: 0-9 */
        if (!n || m < mcs)
            mcs = m;
        best = n + 1;
    }
    *streams = best;
    return mcs;
}

u32 wl_ht_best(u32 ours, u32 theirs, u32 *streams)
{
    u32 both = ours & theirs, n = 0;

    while (n < 4 && (both >> (8 * n) & 0xFF) == 0xFF)
        n++;
    if (!n && (both & 0xFF)) {          /* fewer than all eight on one stream */
        u32 m = 7;

        while (m && !(both >> m & 1))
            m--;
        *streams = 1;
        return m;
    }
    *streams = n;
    return 7;
}
