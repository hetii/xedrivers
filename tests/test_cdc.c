/* Native tests for the CDC descriptor parser, against the real RTL8156B bytes in
 * fixtures.h. Built and run on the host by `make test`; no console needed. */

#include <stdio.h>
#include <string.h>

#include "../src/cdc.h"
#include "fixtures.h"

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

static void test_ecm(void)
{
    CDC_LAYOUT l;

    CHECK(cdc_parse_config(rtl8156_config3_ecm, sizeof(rtl8156_config3_ecm), &l));
    CHECK(l.config_value == 3);
    CHECK(l.subclass == CDC_SUBCLASS_ECM);
    CHECK(l.comm_interface == 0);
    CHECK(l.data_interface == 1);
    CHECK(l.data_alt == 1);
    CHECK(l.ep_in == 0x81);
    CHECK(l.ep_out == 0x02);
    CHECK(l.ep_intr == 0x83);
    CHECK(l.in_max == 512);
    CHECK(l.out_max == 512);
    CHECK(l.intr_max == 16);
    CHECK(l.mac_string != 0);
}

static void test_ncm(void)
{
    CDC_LAYOUT l;

    CHECK(cdc_parse_config(rtl8156_config2_ncm, sizeof(rtl8156_config2_ncm), &l));
    CHECK(l.config_value == 2);
    CHECK(l.subclass == CDC_SUBCLASS_NCM);
    CHECK(l.comm_interface == 0);
    CHECK(l.data_interface == 1);
    CHECK(l.data_alt == 1);
    CHECK(l.ep_in == 0x81);
    CHECK(l.ep_out == 0x02);
    CHECK(l.ep_intr == 0x83);
}

static void test_bad_input(void)
{
    CDC_LAYOUT l;
    unsigned char truncated[4] = { 0x09, 0x02, 0x56, 0x00 };

    CHECK(!cdc_parse_config(truncated, sizeof(truncated), &l));
    CHECK(!cdc_parse_config(rtl8156_config3_ecm, 5, &l));
}

static void test_mac(void)
{
    /* "001122334455" as a USB string descriptor, UTF-16LE. */
    unsigned char s[2 + 12 * 2];
    const char *hex = "001122334455";
    int i;
    unsigned char mac[6];
    unsigned char expect[6] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55 };

    s[0] = sizeof(s);
    s[1] = 0x03;
    for (i = 0; i < 12; i++) {
        s[2 + i * 2] = (unsigned char)hex[i];
        s[2 + i * 2 + 1] = 0;
    }
    CHECK(cdc_parse_mac(s, sizeof(s), mac));
    CHECK(memcmp(mac, expect, 6) == 0);
}

static void test_notification(void)
{
    CDC_NOTE n;
    /* NETWORK_CONNECTION, connected: A1 00 0001 iface=1 0000 */
    unsigned char link_up[8] = { 0xA1, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00 };
    unsigned char link_down[8] = { 0xA1, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00 };
    /* CONNECTION_SPEED_CHANGE: A1 2A 0000 iface 0008 then 2.5G down/up */
    unsigned char speed[16] = {
        0xA1, 0x2A, 0x00, 0x00, 0x01, 0x00, 0x08, 0x00,
        0x00, 0xF2, 0x05, 0x95, 0x00, 0xF2, 0x05, 0x95
    };

    CHECK(cdc_parse_notification(link_up, 8, &n) == 8);
    CHECK(n.kind == CDC_NOTE_LINK && n.link_up == 1);
    CHECK(cdc_parse_notification(link_down, 8, &n) == 8);
    CHECK(n.kind == CDC_NOTE_LINK && n.link_up == 0);
    CHECK(cdc_parse_notification(speed, 16, &n) == 16);
    CHECK(n.kind == CDC_NOTE_SPEED && n.down_bps == 0x9505F200);
}

/* --- NCM ---------------------------------------------------------------- */

static unsigned char seen[8][1600];
static unsigned seen_len[8];
static int seen_count;

static void collect(const u8 *frame, u32 length, void *context)
{
    (void)context;
    if (seen_count < 8) {
        memcpy(seen[seen_count], frame, length);
        seen_len[seen_count] = length;
    }
    seen_count++;
}

static void test_ncm_roundtrip(void)
{
    static unsigned char buf[16384];
    unsigned char f[3][1514];
    unsigned lens[3] = { 60, 1514, 333 };
    NCM_PARAMS p = { 1, 16384, 4, 0, 4, 16384, 4, 2, 4, 0 };
    NCM_TX tx;
    unsigned i, j, total;

    for (i = 0; i < 3; i++)
        for (j = 0; j < lens[i]; j++)
            f[i][j] = (unsigned char)(i * 7 + j);

    /* NDP after the datagrams first, then the default layout below. */
    cdc_ncm_tx_begin(&tx, buf, sizeof(buf), &p, 5, 1);
    for (i = 0; i < 3; i++)
        CHECK(cdc_ncm_tx_add(&tx, f[i], lens[i]));
    total = cdc_ncm_tx_finish(&tx, 512);
    seen_count = 0;
    CHECK(cdc_ncm_parse_ntb(buf, total, collect, 0) == 3);
    for (i = 0; i < 3; i++)
        CHECK(seen_len[i] == lens[i] && memcmp(seen[i], f[i], lens[i]) == 0);
    CHECK((buf[10] | buf[11] << 8) > 12);        /* wNdpIndex points past data */

    cdc_ncm_tx_begin(&tx, buf, sizeof(buf), &p, 5, 0);
    for (i = 0; i < 3; i++)
        CHECK(cdc_ncm_tx_add(&tx, f[i], lens[i]));
    total = cdc_ncm_tx_finish(&tx, 512);
    CHECK(total % 512 != 0);
    CHECK(buf[0] == 'N' && buf[1] == 'C' && buf[2] == 'M' && buf[3] == 'H');

    /* Every datagram sits at offset % 4 == 2, as the parameters asked. */
    for (i = 0; i < 3; i++)
        CHECK((buf[12 + 8 + 4 * i] | buf[12 + 8 + 4 * i + 1] << 8) % 4 == 2);

    seen_count = 0;
    CHECK(cdc_ncm_parse_ntb(buf, total, collect, 0) == 3);
    CHECK(seen_count == 3);
    for (i = 0; i < 3; i++) {
        CHECK(seen_len[i] == lens[i]);
        CHECK(memcmp(seen[i], f[i], lens[i]) == 0);
    }
}

static void test_ncm_limits(void)
{
    static unsigned char buf[2048];
    unsigned char frame[1514] = { 0 };
    NCM_PARAMS p = { 1, 2048, 4, 0, 4, 2048, 4, 0, 4, 2 };
    NCM_TX tx;

    cdc_ncm_tx_begin(&tx, buf, sizeof(buf), &p, 0, 0);
    CHECK(cdc_ncm_tx_add(&tx, frame, 1514));
    CHECK(!cdc_ncm_tx_add(&tx, frame, 1514));   /* does not fit */
    CHECK(cdc_ncm_tx_add(&tx, frame, 60));
    CHECK(!cdc_ncm_tx_add(&tx, frame, 60));     /* max datagrams 2 */

    /* A corrupt block is refused, not walked. */
    buf[0] = 'X';
    CHECK(cdc_ncm_parse_ntb(buf, 2048, collect, 0) == -1);
}

int main(void)
{
    test_ncm_roundtrip();
    test_ncm_limits();
    test_ecm();
    test_ncm();
    test_bad_input();
    test_mac();
    test_notification();

    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("all cdc tests passed\n");
    return 0;
}
