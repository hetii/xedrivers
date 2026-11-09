/* btusb_find and the host's parsing (hci.c) on the host, with descriptors and events as
 * controllers send them. */

#include <stdio.h>
#include <string.h>

#include "btusb.h"
#include "host.h"

static int failures;

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* What btusb.c calls of the core: never reached by these tests. */
void  usb_prepare(URB *u, USB_DEV *d, void *e, URB_COMPLETION f, void *b, u32 l, const u8 *s)
{ (void)u; (void)d; (void)e; (void)f; (void)b; (void)l; (void)s; }
void  usb_submit(URB *u) { (void)u; }
void  usb_wake(void) { }
u32   usb_control(USB_DEV *d, const u8 s[8], void *b, u32 l, u32 *a, u32 t)
{ (void)d; (void)s; (void)b; (void)l; (void)a; (void)t; return 1; }
void *usb_open(USB_DEV *d, u32 t, u32 a, u32 m, u32 i) { (void)d; (void)t; (void)a; (void)m; (void)i; return 0; }
void *MmAllocatePhysicalMemory(u32 f, u32 s, u32 p) { (void)f; (void)s; (void)p; return 0; }

/* Intel AX200's Bluetooth (8087:0029), read on the PC: HCI on interface 0, SCO on 1. */
static const u8 ax200[] = {
    0x09, 0x02, 0xc8, 0x00, 0x02, 0x01, 0x00, 0xe0, 0x32,
    0x09, 0x04, 0x00, 0x00, 0x03, 0xe0, 0x01, 0x01, 0x00,
    0x07, 0x05, 0x81, 0x03, 0x40, 0x00, 0x01,
    0x07, 0x05, 0x02, 0x02, 0x40, 0x00, 0x01,
    0x07, 0x05, 0x82, 0x02, 0x40, 0x00, 0x01,
    0x09, 0x04, 0x01, 0x00, 0x02, 0xe0, 0x01, 0x01, 0x00,
    0x07, 0x05, 0x03, 0x01, 0x00, 0x00, 0x01,
    0x07, 0x05, 0x83, 0x01, 0x00, 0x00, 0x01,
    0x09, 0x04, 0x01, 0x01, 0x02, 0xe0, 0x01, 0x01, 0x00,
    0x07, 0x05, 0x03, 0x01, 0x09, 0x00, 0x01,
    0x07, 0x05, 0x83, 0x01, 0x09, 0x00, 0x01,};

/* No HCI interface: a vendor one only. */
static const u8 vendor[] = {
    9, 2, 25, 0, 1, 1, 0, 0x80, 50,
    9, 4, 0, 0, 1, 0xFF, 0xFF, 0xFF, 0,
    7, 5, 0x81, 2, 0, 2, 0,
};

static void test_find(void)
{
    BTUSB_EPS e;

    CHECK(btusb_find(ax200, sizeof(ax200), &e));
    CHECK(e.interface == 0);
    CHECK(e.event == 0x81 && e.event_max == 64 && e.event_interval == 1);
    CHECK(e.acl_in == 0x82 && e.acl_out == 0x02 && e.acl_max == 64);
    CHECK(!btusb_find(vendor, sizeof(vendor), &e));
    CHECK(!btusb_find(ax200, 32, &e));          /* cut before the bulk IN endpoint */
}

static void test_eir_name(void)
{
    static const u8 eir[] = { 2, 0x01, 0x06, 7, 0x09, '0', '2', '7', '5', '7', '0', 0 };
    static const u8 shortened[] = { 4, 0x08, 'H', 'S', 'X' };
    static const u8 broken[] = { 9, 0x09, 'a', 'b' };
    char name[32];

    CHECK(hci_eir_name(eir, sizeof(eir), name, sizeof(name)) == 6 && !strcmp(name, "027570"));
    CHECK(hci_eir_name(shortened, sizeof(shortened), name, sizeof(name)) == 3 &&
          !strcmp(name, "HSX"));
    CHECK(hci_eir_name(broken, sizeof(broken), name, sizeof(name)) == 0);
    CHECK(hci_eir_name(eir, sizeof(eir), name, 4) == 3 && !strcmp(name, "027"));
}

static void test_inquiry_results(void)
{
    /* Extended Inquiry Result: one response, address C6:52:BA:8A:3F:4F, class 0x240404
     * (audio, headset), RSSI -60, EIR with the name. */
    u8 ext[3 + 14 + 240];
    static const u8 rssi[] = { 0x22, 15, 1, 0x4F, 0x3F, 0x8A, 0xBA, 0x52, 0xC6, 1, 0,
                               0x04, 0x04, 0x24, 0, 0, (u8)-70 };
    static const u8 plain[] = { 0x02, 15, 1, 0x55, 0x48, 0xF7, 0x71, 0xEB, 0x50, 1, 0, 0,
                                0x0C, 0x01, 0x7C, 0, 0 };

    memset(ext, 0, sizeof(ext));
    ext[0] = 0x2F;
    ext[1] = 255;
    ext[2] = 1;
    memcpy(ext + 3, "\x4F\x3F\x8A\xBA\x52\xC6", 6);
    ext[3 + 6] = 1;                 /* page scan repetition */
    ext[3 + 8] = 0x04;
    ext[3 + 9] = 0x04;
    ext[3 + 10] = 0x24;
    ext[3 + 13] = (u8)-60;
    memcpy(ext + 17, "\x07\x09" "027570", 8);

    memset((void *)bt_found, 0, sizeof(bt_found));
    bt_stats.found = 0;
    hci_inquiry_result(ext, 2 + 255);
    CHECK(bt_stats.found == 1);
    CHECK(!memcmp((const void *)bt_found[0].address, "\xC6\x52\xBA\x8A\x3F\x4F", 6));
    CHECK(bt_found[0].device_class == 0x240404);
    CHECK(bt_found[0].rssi == -60);
    CHECK(!strcmp((const char *)bt_found[0].name, "027570"));

    hci_inquiry_result(rssi, sizeof(rssi));     /* the same device again, by RSSI result */
    CHECK(bt_stats.found == 1 && bt_found[0].seen == 2 && bt_found[0].rssi == -70);

    hci_inquiry_result(plain, sizeof(plain));   /* another, plain result: class at 9 */
    CHECK(bt_stats.found == 2);
    CHECK(!memcmp((const void *)bt_found[1].address, "\x50\xEB\x71\xF7\x48\x55", 6));
    CHECK(bt_found[1].device_class == 0x7C010C);

    hci_inquiry_result(plain, 10);              /* cut short: nothing read */
    CHECK(bt_stats.found == 2 && bt_found[1].seen == 1);
}

int main(void)
{
    test_find();
    test_eir_name();
    test_inquiry_results();
    if (failures) {
        printf("bluetooth: %d failed\n", failures);
        return 1;
    }
    printf("bluetooth: all passed\n");
    return 0;
}
