/* Native test of the PHY advertisement mapping (rtl_advert_regs): the same three
 * register values Linux r8152 writes for each speed set, checked against what the
 * twin card under r8152 actually held (tests/rtlreg.py dump):
 *   advertise all -> ADVERTISE 0x1de1, CTRL1000 0x0200, 10GBT_CTRL 0x0081. */

#include <stdio.h>

#include "rtl.h"
#include "usb.h"

/* rtl.c's transfer side is not reached by this test. */
volatile USB_STATS usb_stats;
u32 usb_control(USB_DEV *dev, const u8 setup[8], void *b, u32 l, u32 *a, u32 t)
{ (void)dev; (void)setup; (void)b; (void)l; (void)a; (void)t; return STATUS_UNSUCCESSFUL; }
void *net_dma_scratch(u32 bytes) { (void)bytes; return 0; }

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                      failures++; } } while (0)

int main(void)
{
    /* Starting from the card's 10/100-only state (pause bits and selector kept). */
    u32 adv = 0x0de1 & ~0x0180u, c1000 = 0x0000, c10g = 0x0001;

    rtl_advert_regs(RTL_ADV_ALL, &adv, &c1000, &c10g);
    CHECK(adv == 0x0de1);
    CHECK(c1000 == 0x0200);
    CHECK(c10g == 0x0081);

    rtl_advert_regs(RTL_ADV_10_HALF | RTL_ADV_10_FULL | RTL_ADV_100_HALF |
                    RTL_ADV_100_FULL, &adv, &c1000, &c10g);
    CHECK(adv == 0x0de1);
    CHECK(c1000 == 0x0000);
    CHECK(c10g == 0x0001);

    if (failures) { printf("%d check(s) failed\n", failures); return 1; }
    printf("all rtl tests passed\n");
    return 0;
}
