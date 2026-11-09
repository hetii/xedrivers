/* btusb_find and the host's parsing (hci.c) on the host, with descriptors and events as
 * controllers send them. */

#include <stdio.h>
#include <string.h>

#include "btusb.h"
#include "host.h"
#include "sbc.h"
#include "a2dp.h"

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
void  bt_load_keys(void) { }
u32   bt_save_keys(void) { return 0; }

/* A transport that keeps what was sent. */
static u8 sent[16][64];
static u32 sent_length[16], sent_count;

static u32 take_command(void *ctx, const u8 *p, u32 n)
{
    u32 i;

    (void)ctx;
    if (sent_count < 16) {
        for (i = 0; i < n && i < 64; i++)
            sent[sent_count][i] = p[i];
        sent_length[sent_count++] = n;
    }
    return 0;
}

static u8 acl_sent[16][64];
static u32 acl_sent_count;

static u32 take_acl(void *ctx, const u8 *p, u32 n)
{
    u32 i;

    (void)ctx;
    if (acl_sent_count < 16) {
        for (i = 0; i < n && i < 64; i++)
            acl_sent[acl_sent_count][i] = p[i];
        acl_sent_count++;
    }
    return 0;
}

static const BT_TRANSPORT fake = { 0, take_command, take_acl };

static void event(const u8 *e, u32 n)
{
    bt_event(e, n);
    bt_tick();
}

/* A Command Complete for `opcode`, status 0, `credits` given back. */
static void complete_ok(u32 opcode)
{
    u8 e[12] = { 0x0E, 4, 1, (u8)opcode, (u8)(opcode >> 8), 0 };

    if (opcode == 0x1009) {                     /* Read_BD_ADDR: 68:8F:C9:33:9C:7E */
        static const u8 a[6] = { 0x7E, 0x9C, 0x33, 0xC9, 0x8F, 0x68 };

        memcpy(e + 6, a, 6);
        e[1] = 10;
    }
    event(e, 2u + e[1]);
}

static u32 last_opcode(void)
{
    return sent_count ? (u32)(sent[sent_count - 1][0] | sent[sent_count - 1][1] << 8) : 0;
}

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

static void test_pair_line(void)
{
    u8 address[6], key[16];
    u32 kind;

    CHECK(bt_parse_pair("C6:52:BA:8A:3F:4F 00112233445566778899AABBCCDDEEFF audio",
                        address, key, &kind));
    CHECK(!memcmp(address, "\xC6\x52\xBA\x8A\x3F\x4F", 6));
    CHECK(key[15] == 0x00 && key[0] == 0xFF && key[14] == 0x11);
    CHECK(kind == BT_KIND_AUDIO);
    CHECK(bt_parse_pair("c6:52:ba:8a:3f:4f\t00112233445566778899aabbccddeeff hid",
                        address, key, &kind) && kind == BT_KIND_HID);
    CHECK(!bt_parse_pair("C6:52:BA:8A:3F 00112233445566778899AABBCCDDEEFF", address, key,
                         &kind));
    CHECK(!bt_parse_pair("C6:52:BA:8A:3F:4F 0011223344556677", address, key, &kind));
}

/* Bring-up to the end, then a connection we ask for and a Just Works pairing. */
static void test_connect_and_pair(void)
{
    static const u32 bring_up[9] = { 0x0C03, 0x1009, 0x1005, 0x0C01, 0x0C24, 0x0C13,
                                     0x0C45, 0x0C56, 0x0C1A };
    static const u8 headset[6] = { 0xC6, 0x52, 0xBA, 0x8A, 0x3F, 0x4F };
    u8 e[32];
    u32 i;

    sent_count = 0;
    bt_attach(&fake);
    bt_tick();
    for (i = 0; i < 9; i++) {
        CHECK(last_opcode() == bring_up[i]);
        complete_ok(bring_up[i]);
    }
    CHECK(bt_stats.state == BT_STATE_READY);
    CHECK(((volatile u8 *)bt_stats.address)[0] == 0x68 && ((volatile u8 *)bt_stats.address)[5] == 0x7E);

    CHECK(bt_connect(headset) == 0);
    bt_tick();
    CHECK(last_opcode() == 0x0405);
    CHECK(sent[sent_count - 1][3] == 0x4F && sent[sent_count - 1][8] == 0xC6);

    /* Command Status ok, then Connection Complete: handle 0x0B, ACL */
    { static const u8 s[] = { 0x0F, 4, 0, 1, 0x05, 0x04 }; event(s, sizeof(s)); }
    memset(e, 0, sizeof(e));
    e[0] = 0x03; e[1] = 11; e[2] = 0; e[3] = 0x0B; e[4] = 0;
    for (i = 0; i < 6; i++)
        e[5 + i] = headset[5 - i];
    e[11] = 1;
    event(e, 13);
    CHECK(bt_conns[0].state == BT_CONN_AUTH && bt_conns[0].handle == 0x0B);
    CHECK(last_opcode() == 0x0411);
    { static const u8 s[] = { 0x0F, 4, 0, 1, 0x11, 0x04 }; event(s, sizeof(s)); }

    /* no key yet: a negative reply */
    e[0] = 0x17; e[1] = 6;
    for (i = 0; i < 6; i++)
        e[2 + i] = headset[5 - i];
    event(e, 8);
    CHECK(last_opcode() == 0x040C);
    complete_ok(0x040C);

    /* IO capability: no input, no output, general bonding */
    e[0] = 0x31;
    event(e, 8);
    CHECK(last_opcode() == 0x042B && sent[sent_count - 1][9] == 3 &&
          sent[sent_count - 1][11] == 4);
    complete_ok(0x042B);

    /* user confirmation: yes */
    e[0] = 0x33; e[1] = 10;
    event(e, 12);
    CHECK(last_opcode() == 0x042C);
    complete_ok(0x042C);

    /* the key, then authentication done, then encryption on */
    e[0] = 0x18; e[1] = 23;
    for (i = 0; i < 16; i++)
        e[8 + i] = (u8)(0xA0 + i);
    e[24] = 4;
    event(e, 25);
    CHECK(bt_stats.pairings == 1 && bt_keys_changed);
    CHECK(bt_key_for(headset) && bt_key_for(headset)->key[0] == 0xA0);
    { static const u8 a[] = { 0x06, 3, 0, 0x0B, 0 }; event(a, sizeof(a)); }
    CHECK(last_opcode() == 0x0413 && bt_conns[0].state == BT_CONN_ENCRYPTING);
    { static const u8 s[] = { 0x0F, 4, 0, 1, 0x13, 0x04 }; event(s, sizeof(s)); }
    { static const u8 c[] = { 0x08, 4, 0, 0x0B, 0, 1 }; event(c, sizeof(c)); }
    CHECK(bt_conns[0].state == BT_CONN_SECURE && bt_conns[0].encrypted);

    /* the next time, the key is given */
    e[0] = 0x17; e[1] = 6;
    for (i = 0; i < 6; i++)
        e[2 + i] = headset[5 - i];
    event(e, 8);
    CHECK(last_opcode() == 0x040B && sent[sent_count - 1][9] == 0xA0);

    /* gone */
    { static const u8 d[] = { 0x05, 4, 0, 0x0B, 0, 0x13 }; event(d, sizeof(d)); }
    CHECK(bt_conns[0].state == BT_CONN_FREE);
    bt_detach();
}

static u32 opened, received, closed;

static void ch_open(L2CAP_CHANNEL *ch) { (void)ch; opened++; }
static void ch_data(L2CAP_CHANNEL *ch, const u8 *d, u32 n) { (void)ch; (void)d; received += n; }
static void ch_closed(L2CAP_CHANNEL *ch, u32 was) { (void)ch; (void)was; closed++; }
static const L2CAP_USER user = { ch_open, ch_data, ch_closed };

/* One ACL packet from the device on handle 0x0B: an L2CAP frame to `cid`. */
static void acl(u32 cid, const u8 *p, u32 n)
{
    u8 a[80];

    a[0] = 0x0B; a[1] = 0x20; a[2] = (u8)(n + 4); a[3] = 0;
    a[4] = (u8)n; a[5] = 0; a[6] = (u8)cid; a[7] = (u8)(cid >> 8);
    memcpy(a + 8, p, n);
    bt_acl_in(a, n + 8);
    bt_tick();
}

/* The device opens a channel to a PSM we listen on, both sides configure, data flows. */
static void test_l2cap_incoming(void)
{
    static const u32 bring_up[9] = { 0x0C03, 0x1009, 0x1005, 0x0C01, 0x0C24, 0x0C13,
                                     0x0C45, 0x0C56, 0x0C1A };
    static const u8 buffer[] = { 0x0E, 11, 1, 0x05, 0x10, 0, 0xFD, 0x03, 0x40, 9, 0, 0, 0 };
    u32 i, local;
    u8 *r;

    sent_count = acl_sent_count = 0;
    opened = received = closed = 0;
    bt_attach(&fake);
    bt_tick();
    for (i = 0; i < 9; i++) {
        if (bring_up[i] == 0x1005)
            event(buffer, sizeof(buffer));
        else
            complete_ok(bring_up[i]);
    }
    CHECK(bt_stats.acl_mtu == 1021 && bt_stats.acl_slots == 9);
    memset((void *)&bt_conns[0], 0, sizeof(BT_CONN));
    memcpy((void *)bt_conns[0].address, "\xC6\x52\xBA\x8A\x3F\x4F", 6);
    bt_conns[0].state = BT_CONN_SECURE;
    bt_conns[0].handle = 0x0B;
    CHECK(l2cap_listen(0x0019, &user, 0) == 0);

    /* Connection Request: PSM 0x19, their CID 0x0041 */
    { static const u8 s[] = { 0x02, 7, 4, 0, 0x19, 0, 0x41, 0 }; acl(1, s, sizeof(s)); }
    CHECK(acl_sent_count == 2);                 /* the response, then our config request */
    r = acl_sent[0];
    CHECK(r[6] == 1 && r[8] == 0x03 && r[9] == 7);          /* signalling, conn rsp, id 7 */
    local = (u32)(r[12] | r[13] << 8);
    CHECK(local >= 0x40 && r[14] == 0x41 && r[16] == 0);    /* their CID, success */
    r = acl_sent[1];
    CHECK(r[8] == 0x04 && r[12] == 0x41 && r[16] == 1 && (r[18] | r[19] << 8) == 1017);

    /* their config request, MTU 895; then their answer to ours */
    {
        u8 s[] = { 0x04, 9, 8, 0, 0, 0, 0, 0, 1, 2, 0x7F, 0x03 };

        s[4] = (u8)local;
        s[5] = (u8)(local >> 8);
        acl(1, s, sizeof(s));
    }
    CHECK(acl_sent[2][8] == 0x05 && acl_sent[2][12] == 0x41 && acl_sent[2][16] == 0);
    CHECK(!opened);
    {
        u8 s[] = { 0x05, r[9], 6, 0, 0, 0, 0, 0, 0, 0 };

        s[4] = (u8)local;
        s[5] = (u8)(local >> 8);
        acl(1, s, sizeof(s));
    }
    CHECK(opened == 1);
    CHECK(l2cap_channels[local - 0x40].remote_mtu == 895);

    /* data on our CID goes to the user; what we send goes to theirs */
    { static const u8 d[] = { 1, 2, 3, 4, 5 }; acl(local, d, sizeof(d)); }
    CHECK(received == 5);
    { static const u8 d[] = { 9, 9 }; CHECK(l2cap_send(&l2cap_channels[local - 0x40], d, 2) == 0); }
    CHECK(acl_sent[acl_sent_count - 1][6] == 0x41 && acl_sent[acl_sent_count - 1][8] == 9);

    /* an echo is answered with the same data */
    { static const u8 s[] = { 0x08, 3, 2, 0, 'h', 'i' }; acl(1, s, sizeof(s)); }
    r = acl_sent[acl_sent_count - 1];
    CHECK(r[8] == 0x09 && r[9] == 3 && r[12] == 'h' && r[13] == 'i');

    /* a PSM nobody listens on is refused */
    { static const u8 s[] = { 0x02, 4, 4, 0, 0x03, 0, 0x42, 0 }; acl(1, s, sizeof(s)); }
    r = acl_sent[acl_sent_count - 1];
    CHECK(r[8] == 0x03 && r[16] == 2);

    /* credits: 9, used as sent, back with Number Of Completed Packets */
    for (i = 0; i < 20; i++) {
        static const u8 d[] = { 0 };

        if (l2cap_send(&l2cap_channels[local - 0x40], d, 1))
            break;
    }
    CHECK(i < 9);
    { static const u8 n[] = { 0x13, 5, 1, 0x0B, 0, 2, 0 }; event(n, sizeof(n)); }
    { static const u8 d[] = { 0 }; CHECK(l2cap_send(&l2cap_channels[local - 0x40], d, 1) == 0); }

    /* the link goes: the channel closes */
    bt_l2cap_gone(&bt_conns[0]);
    CHECK(closed == 1 && l2cap_channels[local - 0x40].state == L2CAP_FREE);
    bt_detach();
}

/* SBC: lengths, the header, its CRC, and that silence stays silent. A decode against a
 * reference (gst sbcdec) gave 65 dB SNR in every mode, 2026-11-09. */
static void test_sbc(void)
{
    static s16 pcm[128 * 2];
    u8 out[SBC_FRAME_MAX];
    SBC s;
    u32 n;

    sbc_init(&s, SBC_RATE_44100, 16, SBC_JOINT, SBC_LOUDNESS, 53);
    CHECK(sbc_samples(&s) == 128 && sbc_frame_length(&s) == 119);
    n = sbc_encode(&s, pcm, out);
    /* 44.1 kHz, 16 blocks, joint, loudness, 8 subbands: 10 11 11 0 1 */
    CHECK(n == 119 && out[0] == 0x9C && out[1] == 0xBD && out[2] == 53);
    CHECK(out[3] == 0x7D);                      /* CRC-8 of 0xBD 0x35 and 72 zero bits */
    CHECK(out[4] == 0);                         /* silence: nothing joined, scale factors 0 */
    sbc_init(&s, SBC_RATE_44100, 16, SBC_DUAL, SBC_LOUDNESS, 38);    /* SBC-XQ 552 kbit/s */
    CHECK(sbc_frame_length(&s) == 4 + 8 + 2 * 16 * 38 / 8);
    sbc_init(&s, SBC_RATE_48000, 16, SBC_MONO, SBC_SNR, 31);
    CHECK(sbc_frame_length(&s) == 4 + 4 + 62);
}

/* The configuration from a sink's SBC capabilities. */
static void test_a2dp_choose(void)
{
    SBC_CAPS all = { { 0xFF, 0xFF, 2, 53 } }, out;
    SBC_CAPS joint44 = { { 0x21, 0x15, 2, 53 } };   /* 44.1k joint, 16 blocks, 8sb, loudness */
    SBC_CAPS four = { { 0xFF, 0xF8, 2, 53 } };      /* 4 subbands only */
    u32 bp = 0;

    /* everything: 48 kHz, dual channel (twice the bits at the same bitpool), 16 blocks */
    CHECK(a2dp_choose(&all, 2, 53, &out, &bp));
    CHECK(out.b[0] == 0x14 && out.b[1] == 0x15 && out.b[2] == 2 && out.b[3] == 53 && bp == 53);
    CHECK(a2dp_choose(&joint44, 2, 53, &out, &bp));
    CHECK(out.b[0] == 0x21 && out.b[1] == 0x15 && bp == 53);
    CHECK(!a2dp_choose(&four, 2, 53, &out, &bp));
    CHECK(!a2dp_choose(&all, 60, 53, &out, &bp));
}

static u8 sdp_sent[8][600];
static u32 sdp_sent_len[8], sdp_count;

/* SDP on a channel opened by the device: a ServiceSearchAttributeRequest for AudioSource. */
static void test_sdp(void)
{
    u32 local;
    u8 *r;

    (void)sdp_sent; (void)sdp_sent_len; (void)sdp_count;
    acl_sent_count = 0;
    bt_attach(&fake);
    bt_tick();
    {
        static const u32 bring_up[9] = { 0x0C03, 0x1009, 0x1005, 0x0C01, 0x0C24, 0x0C13,
                                         0x0C45, 0x0C56, 0x0C1A };
        static const u8 buffer[] = { 0x0E, 11, 1, 0x05, 0x10, 0, 0xFD, 0x03, 0x40, 9, 0, 0, 0 };
        u32 i;

        for (i = 0; i < 9; i++) {
            if (bring_up[i] == 0x1005)
                event(buffer, sizeof(buffer));
            else
                complete_ok(bring_up[i]);
        }
    }
    memset((void *)&bt_conns[0], 0, sizeof(BT_CONN));
    bt_conns[0].state = BT_CONN_SECURE;
    bt_conns[0].handle = 0x0B;
    { static const u8 s[] = { 0x02, 1, 4, 0, 0x01, 0, 0x45, 0 }; acl(1, s, sizeof(s)); }
    local = (u32)(acl_sent[0][12] | acl_sent[0][13] << 8);
    CHECK(acl_sent[0][16] == 0);                    /* accepted */
    {
        u8 s[] = { 0x04, 2, 8, 0, 0, 0, 0, 0, 1, 2, 0x00, 0x04 };   /* MTU 1024 */

        s[4] = (u8)local; s[5] = (u8)(local >> 8);
        acl(1, s, sizeof(s));
        { u8 c[] = { 0x05, acl_sent[1][9], 6, 0, (u8)local, (u8)(local >> 8), 0, 0, 0, 0 };
          acl(1, c, sizeof(c)); }
    }
    {
        /* search {AudioSource}, max 0x0400 bytes, attributes 0x0000-0xFFFF */
        static const u8 q[] = { 0x06, 0x00, 0x01, 0x00, 0x0F,
                                0x35, 0x03, 0x19, 0x11, 0x0A, 0x04, 0x00,
                                0x35, 0x05, 0x0A, 0x00, 0x00, 0xFF, 0xFF, 0x00 };

        acl(local, q, sizeof(q));
    }
    r = acl_sent[acl_sent_count - 1];
    CHECK(r[6] == 0x45 && r[8] == 0x07 && r[10] == 0x01);           /* to its CID, the answer */
    CHECK(r[15] == 0x36);                                           /* a sequence of records */
    CHECK(sdp_stats.requests == 1 && sdp_stats.errors == 0);
    {
        /* the same, limited to 16 bytes: a part and a continuation state */
        static const u8 q[] = { 0x06, 0x00, 0x02, 0x00, 0x0F,
                                0x35, 0x03, 0x19, 0x11, 0x0A, 0x00, 0x10,
                                0x35, 0x05, 0x0A, 0x00, 0x00, 0xFF, 0xFF, 0x00 };

        acl(local, q, sizeof(q));
    }
    r = acl_sent[acl_sent_count - 1];
    CHECK(r[13] == 0 && r[14] == 0x10 && r[15 + 16] == 2 && sdp_stats.continued == 1);
    bt_detach();
}

int main(void)
{
    test_find();
    test_eir_name();
    test_inquiry_results();
    test_pair_line();
    test_connect_and_pair();
    test_l2cap_incoming();
    test_sbc();
    test_a2dp_choose();
    test_sdp();
    if (failures) {
        printf("bluetooth: %d failed\n", failures);
        return 1;
    }
    printf("bluetooth: all passed\n");
    return 0;
}
