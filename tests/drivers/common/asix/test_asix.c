/* Native tests for the ASIX configuration descriptor parser. The descriptor is
 * built after the layout AX88179 adapters report (lsusb -v as published for
 * 0b95:1790): one vendor-class interface with interrupt IN 0x81 (8 bytes,
 * bInterval 11), bulk IN 0x82 and bulk OUT 0x03 of 512 bytes at high speed.
 * Built and run on the host by `make test`. */

#include <stdio.h>
#include <string.h>

#include "asix.h"

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

/* asix.c's control path is not exercised here. */
u32 usb_control(USB_DEV *dev, const u8 setup[8], void *buffer, u32 length, u32 *actual,
                u32 timeout_ms)
{
    (void)dev; (void)setup; (void)buffer; (void)length; (void)actual; (void)timeout_ms;
    return 0;
}
void *net_dma_scratch(u32 bytes) { (void)bytes; return 0; }

static const u8 ax179_config[] = {
    0x09, 0x02, 0x27, 0x00, 0x01, 0x01, 0x04, 0xa0, 0x64,
    0x09, 0x04, 0x00, 0x00, 0x03, 0xff, 0xff, 0x00, 0x07,
    0x07, 0x05, 0x81, 0x03, 0x08, 0x00, 0x0b,
    0x07, 0x05, 0x82, 0x02, 0x00, 0x02, 0x00,
    0x07, 0x05, 0x03, 0x02, 0x00, 0x02, 0x00,
};

int main(void)
{
    ASIX_LAYOUT l;

    CHECK(asix_parse_config(ax179_config, sizeof(ax179_config), &l));
    CHECK(l.config_value == 1 && l.interface == 0);
    CHECK(l.ep_in == 0x82 && l.in_max == 512);
    CHECK(l.ep_out == 0x03 && l.out_max == 512);
    CHECK(l.ep_intr == 0x81 && l.intr_max == 8 && l.intr_interval == 11);

    /* Cut before the bulk OUT endpoint: no layout. */
    CHECK(!asix_parse_config(ax179_config, sizeof(ax179_config) - 7, &l));
    /* Not a configuration descriptor. */
    CHECK(!asix_parse_config(ax179_config + 9, sizeof(ax179_config) - 9, &l));

    /* The MII resolution. */
    {
        u32 full = 9;

        CHECK(asix_mii_resolve(0x0000, 0x01E1, 0x01E1, 0x0200, 0x0800, &full) == 0 && !full);
        CHECK(asix_mii_resolve(0x0004, 0x01E1, 0x45E1, 0x0200, 0x3800, &full) == 1000 && full);
        CHECK(asix_mii_resolve(0x0004, 0x01E1, 0x41E1, 0x0200, 0x0000, &full) == 100 && full);
        CHECK(asix_mii_resolve(0x0004, 0x01E1, 0x4081, 0x0000, 0x0000, &full) == 100 && !full);
        CHECK(asix_mii_resolve(0x0004, 0x01E1, 0x4041, 0x0000, 0x0000, &full) == 10 && full);
        CHECK(asix_mii_resolve(0x0004, 0x01E1, 0x4021, 0x0000, 0x0000, &full) == 10 && !full);
    }

    if (failures)
        return 1;
    printf("all asix tests passed\n");
    return 0;
}
