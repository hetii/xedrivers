/* Native tests for the RNDIS messages and descriptor parser. The configuration
 * descriptor is the real one of a Xiaomi phone tethering over USB (2717:ff80, read
 * from sysfs on the PC). Built and run on the host by `make test`. */

#include <stdio.h>
#include <string.h>

#include "rndis.h"

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

/* 2717:ff80 configuration 1: IAD E0/01/03, control interface 0 with CDC header,
 * call management, ACM and union descriptors and interrupt IN 0x81 (8 bytes,
 * bInterval 9), data interface 1 (class 0A) with bulk IN 0x8E and OUT 0x0F. */
static const u8 xiaomi_config[] = {
    0x09, 0x02, 0x4b, 0x00, 0x02, 0x01, 0x04, 0x80, 0xfa,
    0x08, 0x0b, 0x00, 0x02, 0xe0, 0x01, 0x03, 0x07,
    0x09, 0x04, 0x00, 0x00, 0x01, 0xe0, 0x01, 0x03, 0x05,
    0x05, 0x24, 0x00, 0x10, 0x01,
    0x05, 0x24, 0x01, 0x00, 0x01,
    0x04, 0x24, 0x02, 0x00,
    0x05, 0x24, 0x06, 0x00, 0x01,
    0x07, 0x05, 0x81, 0x03, 0x08, 0x00, 0x09,
    0x09, 0x04, 0x01, 0x00, 0x02, 0x0a, 0x00, 0x00, 0x06,
    0x07, 0x05, 0x8e, 0x02, 0x00, 0x02, 0x00,
    0x07, 0x05, 0x0f, 0x02, 0x00, 0x02, 0x00,
};

static void put32(u8 *p, u32 v)
{
    p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}

static u32 get32(const u8 *p)
{
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static void test_config(void)
{
    RNDIS_LAYOUT l;

    CHECK(rndis_parse_config(xiaomi_config, sizeof(xiaomi_config), &l));
    CHECK(l.config_value == 1);
    CHECK(l.comm_interface == 0);
    CHECK(l.data_interface == 1);
    CHECK(l.data_alt == 0);
    CHECK(l.ep_in == 0x8e);
    CHECK(l.ep_out == 0x0f);
    CHECK(l.ep_intr == 0x81);
    CHECK(l.in_max == 512);
    CHECK(l.out_max == 512);
    CHECK(l.intr_max == 8);
    CHECK(l.intr_interval == 9);

    CHECK(rndis_is_control(0xE0, 0x01, 0x03));
    CHECK(rndis_is_control(0x02, 0x02, 0xFF));
    CHECK(!rndis_is_control(0x02, 0x06, 0x00));     /* CDC ECM is not RNDIS */
}

static void test_build(void)
{
    u8 b[64];
    u8 filter[4] = { 0x2d, 0, 0, 0 };

    CHECK(rndis_build_init(b, 1, 0x4000) == 24);
    CHECK(get32(b) == 2 && get32(b + 4) == 24 && get32(b + 8) == 1);
    CHECK(get32(b + 12) == 1 && get32(b + 16) == 0 && get32(b + 20) == 0x4000);

    CHECK(rndis_build_query(b, 2, OID_802_3_PERMANENT_ADDRESS) == 28);
    CHECK(get32(b) == 4 && get32(b + 4) == 28 && get32(b + 12) == 0x01010101);

    CHECK(rndis_build_set(b, 3, OID_GEN_CURRENT_PACKET_FILTER, filter, 4) == 32);
    CHECK(get32(b) == 5 && get32(b + 4) == 32 && get32(b + 16) == 4);
    CHECK(get32(b + 20) == 20 && get32(b + 28) == 0x2d);
}

static void test_replies(void)
{
    u8 b[64];
    RNDIS_REPLY r;

    memset(b, 0, sizeof(b));
    put32(b, 0x80000002); put32(b + 4, 52); put32(b + 8, 1); put32(b + 12, 0);
    put32(b + 32, 10); put32(b + 36, 15000); put32(b + 40, 2);
    CHECK(rndis_parse_reply(b, 52, &r));
    CHECK(r.type == 0x80000002 && r.request == 1 && r.status == 0);
    CHECK(r.max_packets == 10 && r.max_transfer == 15000 && r.alignment == 2);

    /* A query completion carrying a MAC: info at 8 + 16. */
    memset(b, 0, sizeof(b));
    put32(b, 0x80000004); put32(b + 4, 30); put32(b + 8, 2); put32(b + 12, 0);
    put32(b + 16, 6); put32(b + 20, 16);
    memcpy(b + 24, "\x0a\xd9\xe2\xb7\x95\xaf", 6);
    CHECK(rndis_parse_reply(b, 30, &r));
    CHECK(r.info == b + 24 && r.info_length == 6);
    CHECK(rndis_parse_reply(b, 29, &r) == 0);       /* MessageLength past the end */

    put32(b, 0x00000007); put32(b + 4, 20); put32(b + 8, RNDIS_STATUS_MEDIA_CONNECT);
    CHECK(rndis_parse_reply(b, 20, &r));
    CHECK(r.type == 7 && r.status == RNDIS_STATUS_MEDIA_CONNECT);
}

static u32 seen, seen_bytes;
static u8 first;

static void count(const u8 *frame, u32 length, void *context)
{
    (void)context;
    if (!seen)
        first = frame[0];
    seen++;
    seen_bytes += length;
}

static void test_packets(void)
{
    u8 frame[60], buf[400];
    u32 n, i;

    for (i = 0; i < sizeof(frame); i++)
        frame[i] = (u8)(i + 1);
    n = rndis_wrap(buf, sizeof(buf), frame, sizeof(frame));
    CHECK(n == 44 + 60);
    CHECK(get32(buf) == 1 && get32(buf + 4) == 104 && get32(buf + 8) == 36);
    CHECK(get32(buf + 12) == 60 && buf[44] == 1 && buf[103] == 60);
    CHECK(rndis_wrap(buf, 100, frame, sizeof(frame)) == 0);

    /* Two messages in one transfer, then zero padding. */
    memcpy(buf + n, buf, n);
    memset(buf + 2 * n, 0, 16);
    seen = seen_bytes = 0;
    CHECK(rndis_unwrap(buf, 2 * n + 16, count, 0) == 2);
    CHECK(seen == 2 && seen_bytes == 120 && first == 1);

    put32(buf + n + 12, 200);                       /* second one's DataLength lies */
    seen = 0;
    CHECK(rndis_unwrap(buf, 2 * n, count, 0) == -1);
    CHECK(seen == 1);
}

static void test_aligned(void)
{
    u8 frame[61], buf[400];
    u32 n, m, i;

    for (i = 0; i < sizeof(frame); i++)
        frame[i] = (u8)(i + 1);
    memset(buf, 0xAA, sizeof(buf));
    n = rndis_wrap_aligned(buf, sizeof(buf), frame, sizeof(frame), 16);
    CHECK(n == 112);                    /* 44 + 61 = 105, up to 16 */
    CHECK(get32(buf + 4) == 112 && get32(buf + 12) == 61);
    CHECK(buf[105] == 0 && buf[111] == 0);
    m = rndis_wrap_aligned(buf + n, sizeof(buf) - n, frame, sizeof(frame), 16);
    CHECK(m == 112);
    seen = seen_bytes = 0;
    CHECK(rndis_unwrap(buf, n + m, count, 0) == 2);
    CHECK(seen == 2 && seen_bytes == 122);
    CHECK(rndis_wrap_aligned(buf, 110, frame, sizeof(frame), 16) == 0);
    CHECK(rndis_wrap_aligned(buf, sizeof(buf), frame, sizeof(frame), 0) == 105);
}

int main(void)
{
    test_config();
    test_build();
    test_replies();
    test_packets();
    test_aligned();
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("all rndis tests passed\n");
    return 0;
}
