/* Host test for src/wireless/rate.c: the rates an access point shows for a station. */

#include <stdio.h>

#include "wireless.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                                  failures++; } } while (0)

int main(void)
{
    u32 n, m;

    /* what the router (OpenWrt, iw) showed for the cards, 2026-11-10 */
    CHECK(wl_phy_mbps(2, 2, 9, 1) == 866);      /* RTL8822BU: VHT 80, NSS 2, MCS 9, SGI */
    CHECK(wl_phy_mbps(1, 1, 9, 1) == 200);      /* AIC8800: VHT 40, NSS 1, MCS 9, SGI */
    CHECK(wl_phy_mbps(0, 1, 7, 0) == 65);       /* HT 20, MCS 7, long GI */
    CHECK(wl_phy_mbps(0, 1, 7, 1) == 72);       /* 72.2 */
    CHECK(wl_phy_mbps(1, 2, 7, 1) == 300);      /* HT 40, MCS 15, SGI */
    CHECK(wl_phy_mbps(2, 1, 9, 0) == 390);
    CHECK(wl_phy_mbps(3, 2, 9, 1) == 1733);     /* 1733.3 */
    CHECK(wl_phy_mbps(2, 0, 9, 1) == 0 && wl_phy_mbps(4, 1, 9, 1) == 0);

    m = wl_vht_best(0xFFFA, 0xFFFA, &n);        /* MCS 0-9 on two streams, both */
    CHECK(n == 2 && m == 9);
    m = wl_vht_best(0xFFFA, 0xFFFE, &n);        /* the AP takes one */
    CHECK(n == 1 && m == 9);
    m = wl_vht_best(0xFFF9, 0xFFFA, &n);        /* ours 0-8 on the second stream */
    CHECK(n == 2 && m == 8);
    m = wl_vht_best(0xFFFF, 0xFFFA, &n);
    CHECK(n == 0);

    m = wl_ht_best(0xFFFF, 0xFFFF, &n);         /* MCS 0-15 */
    CHECK(n == 2 && m == 7);
    m = wl_ht_best(0xFFFF, 0x00FF, &n);
    CHECK(n == 1 && m == 7);
    m = wl_ht_best(0xFFFF, 0x001F, &n);         /* MCS 0-4 only */
    CHECK(n == 1 && m == 4);

    if (!failures)
        printf("all rate tests passed\n");
    return failures != 0;
}
