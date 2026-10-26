/* Native tests of the AIC8800D80 driver against Linux's own, recorded with usbmon:
 * load-8d80.txt, aic_load_fw loading the firmware (aic_fw.c), and wifi-8d81.txt,
 * aic8800_fdrv bringing the Wi-Fi firmware up and scanning once (aic_wifi.c). Every
 * message the driver sends must be the one Linux sent, byte for byte (the firmware
 * load by length and CRC-32: that recording keeps no firmware), and the chip's
 * answers are replayed from the recording.
 *
 * Needs the firmware files (AIC_FW, /lib/firmware/aic8800D80 by default); without
 * them the test says so and passes. Run by `make test`. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aic.h"
#include "file.h"
#include "wireless.h"

#define EP_IN       ((void *)1)     /* 0x82, confirms and indications */
#define EP_OUT      ((void *)2)     /* 0x02, commands */
#define EP_DATA_IN  ((void *)3)     /* 0x81 */
#define EP_DATA_OUT ((void *)4)     /* 0x01 */
#define EP_BT       ((void *)5)     /* 0x83, the Bluetooth controller's events */

volatile FILE_STATS file_stats;
volatile USB_STATS usb_stats;
u8 net_console_mac[ETH_ALEN];

/* The console's cryptography is not here; the handshake has its own test
 * (tests/wireless/test_wpa.c) and the real answers are checked on the console. */
void XeCryptShaInit(XE_SHA_STATE *s) { (void)s; }
void XeCryptShaUpdate(XE_SHA_STATE *s, const u8 *in, u32 n) { (void)s; (void)in; (void)n; }
void XeCryptShaFinal(XE_SHA_STATE *s, u8 *out, u32 n) { (void)s; memset(out, 0, n); }
void XeCryptHmacShaInit(XE_HMAC_STATE *s, const u8 *k, u32 n) { (void)s; (void)k; (void)n; }
void XeCryptHmacShaUpdate(XE_HMAC_STATE *s, const u8 *in, u32 n) { (void)s; (void)in; (void)n; }
void XeCryptHmacShaFinal(XE_HMAC_STATE *s, u8 *out, u32 n) { (void)s; memset(out, 0, n); }
void XeCryptHmacSha(const u8 *k, u32 kl, const u8 *a, u32 al, const u8 *b, u32 bl,
                    const u8 *c, u32 cl, u8 *out, u32 ol)
{ (void)k; (void)kl; (void)a; (void)al; (void)b; (void)bl; (void)c; (void)cl;
  memset(out, 0, ol); }
void XeCryptAesKey(XE_AES_STATE *s, const u8 *key) { (void)s; (void)key; }
void XeCryptAesEcb(XE_AES_STATE *s, const u8 *in, u8 *out, u32 e)
{ (void)s; (void)e; memcpy(out, in, 16); }
volatile u32 net_console_mac_known;

/* What the network layer saw: the last frame handed up, and the link reports. */
static volatile NET_STATS net_counters;
static u8 up_frame[2048];
static u32 up_length, up_frames, link_reports, last_link;

static u8 up_order[64];             /* the payload's fifth byte, frame by frame */

void net_frame_in(NET_DEV *nd, const u8 *frame, u32 length)
{
    (void)nd;
    up_length = length < sizeof(up_frame) ? length : sizeof(up_frame);
    memcpy(up_frame, frame, up_length);
    if (up_frames < sizeof(up_order) && length > ETH_HLEN + 4)
        up_order[up_frames] = frame[ETH_HLEN + 4];
    up_frames++;
}

static u8 reorder_memory[AIC_REORDER_SLOTS * AIC_REORDER_FRAME];
void *MmAllocatePhysicalMemory(u32 flags, u32 size, u32 protect)
{
    (void)flags; (void)protect;
    return size <= sizeof(reorder_memory) ? reorder_memory : 0;
}

void KeInitializeDpc(KDPC *dpc, PKDEFERRED_ROUTINE routine, void *context)
{
    dpc->DeferredRoutine = routine;
    dpc->DeferredContext = context;
}

/* The reorder timeout's DPC: run on the spot, as if the USB processor took it. */
static KDPC *queued_dpc;
int KeInsertQueueDpc(KDPC *dpc, void *a1, void *a2)
{
    (void)a1; (void)a2;
    queued_dpc = dpc;
    return 1;
}

void net_link_report(NET_DEV *nd, u32 up) { (void)nd; link_reports++; last_link = up; }

void net_add64(volatile u32 *lo, volatile u32 *hi, u32 n)
{
    u32 v = *lo + n;

    if (v < n)
        (*hi)++;
    *lo = v;
}
void net_speed_report(NET_DEV *nd, u32 mbps) { (void)nd; (void)mbps; }
void net_tx_done(NET_DEV *nd) { (void)nd; }

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                      failures++; } } while (0)

/* --- the recording --------------------------------------------------------------- */

static u32 dma_used;        /* each test carves the DMA block for itself */

typedef struct { int out; u32 length; u32 crc; u8 *data; } STEP;
static URB *rx_waiting[AIC_RX_COUNT];
static u32 rx_waiting_count;
static URB *data_rx_waiting[16];
static u32 data_rx_count;
static STEP steps[2048];
static u32 step_count, next_step, outs_matched, first_mismatch = 0xFFFFFFFFu;

static u32 crc32(const u8 *p, u32 n)
{
    u32 c = 0xFFFFFFFFu, i, k;

    for (i = 0; i < n; i++) {
        c ^= p[i];
        for (k = 0; k < 8; k++)
            c = c >> 1 ^ (0xEDB88320u & -(c & 1));
    }
    return ~c;
}

static int load_recording(const char *path)
{
    char line[4096];
    FILE *f = fopen(path, "r");

    step_count = next_step = outs_matched = 0;
    first_mismatch = 0xFFFFFFFFu;
    rx_waiting_count = 0;
    data_rx_count = 0;
    dma_used = 0;
    if (!f)
        return 0;
    while (fgets(line, sizeof(line), f) && step_count < 2048) {
        STEP *s = &steps[step_count];

        if (!strncmp(line, "out ", 4) && strchr(line + 4, ' ')) {
            s->out = 1;
            sscanf(line + 4, "%u %x", &s->length, &s->crc);
            step_count++;
        } else if (!strncmp(line, "out ", 4) || !strncmp(line, "in ", 3)) {
            u32 skip = line[0] == 'o' ? 4 : 3;
            u32 n = (u32)strcspn(line + skip, "\r\n") / 2, i;

            s->out = line[0] == 'o';
            s->data = malloc(n);
            for (i = 0; i < n; i++)
                sscanf(line + skip + 2 * i, "%2hhx", &s->data[i]);
            s->length = n;
            if (s->out)
                s->crc = crc32(s->data, n);
            step_count++;
            continue;
        } else if (0) {
            u32 n = (u32)strcspn(line + 3, "\r\n") / 2, i;

            s->data = malloc(n);
            for (i = 0; i < n; i++)
                sscanf(line + 3 + 2 * i, "%2hhx", &s->data[i]);
            s->length = n;
            step_count++;
        }
    }
    fclose(f);
    return 1;
}

/* --- the USB the driver sees: transfers complete at once ---------------------- */


void usb_prepare(URB *urb, USB_DEV *dev, void *endpoint, URB_COMPLETION done,
                 void *buffer, u32 length, const u8 *setup)
{
    (void)setup;
    urb->Dev = dev;
    urb->Endpoint = endpoint;
    urb->Routine = done;
    urb->Buffer = buffer;
    urb->Length = length;
    urb->Actual = 0;
}

static void deliver_answers(void)
{
    while (next_step < step_count && !steps[next_step].out && rx_waiting_count) {
        STEP *s = &steps[next_step++];
        URB *urb = rx_waiting[--rx_waiting_count];

        memcpy(urb->Buffer, s->data, s->length);
        urb->Actual = s->length;
        urb->Routine(urb, 0);
    }
}

/* The data endpoints: a transmit is kept for the test to look at, a receive is
 * parked until the test feeds it a frame. */
static u8 sent[4096];
static u32 sent_length, sent_count;

void usb_submit(URB *urb)
{
    if (urb->Endpoint == EP_BT)
        return;                 /* the controller answers nothing here */
    if (urb->Endpoint == EP_DATA_OUT) {
        sent_length = urb->Length < sizeof(sent) ? urb->Length : sizeof(sent);
        memcpy(sent, urb->Buffer, sent_length);
        sent_count++;
        urb->Actual = urb->Length;
        urb->Routine(urb, 0);
        return;
    }
    if (urb->Endpoint == EP_DATA_IN) {
        if (data_rx_count < 16)
            data_rx_waiting[data_rx_count++] = urb;
        return;
    }
    if (urb->Endpoint == EP_IN) {
        rx_waiting[rx_waiting_count++] = urb;
        return;
    }
    if (next_step < step_count && steps[next_step].out) {
        STEP *s = &steps[next_step++];

        if (s->length == urb->Length && s->crc == crc32(urb->Buffer, urb->Length))
            outs_matched++;
        else if (first_mismatch == 0xFFFFFFFFu) {
            first_mismatch = outs_matched;
            if (s->data) {
                u32 k;

                printf("  sent:     ");
                for (k = 0; k < urb->Length && k < 48; k++)
                    printf("%02x", ((u8 *)urb->Buffer)[k]);
                printf("\n  expected: ");
                for (k = 0; k < s->length && k < 48; k++)
                    printf("%02x", s->data[k]);
                printf("\n");
            }
        }
    } else if (first_mismatch == 0xFFFFFFFFu) {
        first_mismatch = outs_matched;
    }
    urb->Actual = urb->Length;
    urb->Routine(urb, 0);
    deliver_answers();
}

/* Which phase the chip is in, because endpoint 0x01 is the boot ROM's command
 * channel and the Wi-Fi firmware's data channel. */
static u32 wifi_phase;

void *usb_open(USB_DEV *dev, u32 type, u32 address, u32 max_packet, u32 interval)
{
    (void)dev; (void)type; (void)max_packet; (void)interval;
    if (address == 0x83)
        return EP_BT;
    if (address == 0x81)
        return EP_DATA_IN;
    if (address == 0x01 && wifi_phase)
        return EP_DATA_OUT;
    return address & 0x80 ? EP_IN : EP_OUT;
}

/* Hand one transfer to the parked receive request, as the chip would. */
static void feed_rx(AIC *a, const u8 *buffer, u32 length)
{
    URB *urb;

    (void)a;
    if (!data_rx_count) {
        printf("FAIL: no receive request parked\n");
        failures++;
        return;
    }
    urb = data_rx_waiting[--data_rx_count];
    memcpy(urb->Buffer, buffer, length);
    urb->Actual = length;
    urb->Routine(urb, 0);
}

void usb_wait(u32 ms) { (void)ms; }
void usb_wake(void) { }
void usb_sleep(u32 ms) { (void)ms; }

static u8 dma[512 * 1024];

static void *carve(u32 bytes)
{
    void *p = dma + dma_used;

    dma_used += (bytes + 63) & ~63u;
    return dma_used <= sizeof(dma) ? p : 0;
}

void *net_dma_scratch(u32 bytes) { return carve(bytes); }
u32 net_dma_begin(NET_DEV *nd, u32 total) { (void)nd; (void)total; return 1; }
u32 usb_control(USB_DEV *dev, const u8 setup[8], void *b, u32 l, u32 *a, u32 t)
{ (void)dev; (void)setup; (void)b; (void)l; (void)a; (void)t; return STATUS_UNSUCCESSFUL; }
void *net_dma_alloc(NET_DEV *nd, u32 bytes) { (void)nd; return carve(bytes); }

/* --- files from AIC_FW ------------------------------------------------------------ */

static const char *fw_dir;

void *file_open(const char *name)
{
    char path[512];
    const char *base = strchr(name, '\\');

    snprintf(path, sizeof(path), "%s/%s", fw_dir, base ? base + 1 : name);
    return fopen(path, "rb");
}

u32 file_read(void *handle, void *buffer, u32 length, u32 offset)
{
    fseek((FILE *)handle, offset, SEEK_SET);
    return (u32)fread(buffer, 1, length, (FILE *)handle);
}

void file_close(void *handle) { fclose((FILE *)handle); }

/* xedrivers.ini, as the test wants it to read. */
static const char *setting_ssid = "";

u32 file_setting(const char *name, const char *key, char *out, u32 size)
{
    u32 n = 0;

    (void)name;
    if (!strcmp(key, "wifi_ssid"))
        for (; setting_ssid[n] && n + 1 < size; n++)
            out[n] = setting_ssid[n];
    if (size)
        out[n] = 0;
    return n;
}

/* --- the tests ------------------------------------------------------------------ */

static u32 count_outs(void)
{
    u32 i, outs = 0;

    for (i = 0; i < step_count; i++)
        outs += steps[i].out;
    return outs;
}

static void report(const char *what, u32 outs, volatile AIC_STATS *st)
{
    if (outs_matched != outs)
        printf("  %s: matched %u of %u messages, first difference at %u (step %u/%#x)\n",
               what, outs_matched, outs, first_mismatch, st->step, st->step_status);
}

static void test_load(void)
{
    static USB_DEV dev;
    static NET_DEV nd;
    static AIC a;
    static volatile AIC_STATS st;
    u32 step, outs;

    if (!load_recording("tests/drivers/aic8800/load-8d80.txt")) {
        printf("FAIL: no recording\n");
        failures++;
        return;
    }
    outs = count_outs();
    wifi_phase = 0;
    dev.active = 1;
    nd.usb = &dev;
    nd.st = &net_counters;
    a.nd = &nd;
    a.dev = &dev;
    a.st = &st;
    CHECK(aic_open(&a, 0x01, 0x82) == 0);
    deliver_answers();
    step = aic_load_firmware(&a);
    CHECK(step == 0);
    CHECK(st.step == AIC_STEP_FIRMWARE_STARTED);
    CHECK(st.chip_id == 7);
    CHECK(st.mcu_id == 0);
    CHECK(st.fw_version == 0x06090101u);
    CHECK(st.files_loaded == 4);
    CHECK(st.timeouts == 0);
    CHECK(outs_matched == outs);
    CHECK(next_step == step_count);
    report("load", outs, &st);
    printf("aic load: %u messages as Linux sent them\n", outs_matched);
}

static void test_wifi(void)
{
    static USB_DEV dev;
    static NET_DEV nd;
    static AIC a;
    static volatile AIC_STATS st;
    static const u8 card_mac[6] = { 0x68, 0x8f, 0xc9, 0x33, 0x7a, 0xc7 };
    u32 step, outs, i, named = 0;

    if (!load_recording("tests/drivers/aic8800/wifi-8d81.txt")) {
        printf("FAIL: no recording\n");
        failures++;
        return;
    }
    outs = count_outs();
    wifi_phase = 1;
    dev.active = 1;
    nd.usb = &dev;
    nd.st = &net_counters;
    a.nd = &nd;
    a.dev = &dev;
    a.st = &st;
    step = aic_wifi_start(&a);
    deliver_answers();
    CHECK(step == 0);
    CHECK(st.step == AIC_STEP_WIFI_UP);
    CHECK(memcmp(nd.mac, card_mac, 6) == 0);
    CHECK(st.timeouts == 0);
    CHECK(outs_matched == outs);
    CHECK(next_step == step_count);
    CHECK(!a.scanning);
    CHECK(wl_stats.scans_done == 1);
    CHECK(wl_stats.bss_count >= 3);
    CHECK(wl_stats.bad_frames == 0);
    for (i = 0; i < wl_stats.bss_count; i++) {
        named += wl_bss[i].ssid_len > 0;
        printf("  bss %02x:%02x:%02x:%02x:%02x:%02x ch %3u %4u MHz %4d dBm sec %02x '%s'\n",
               wl_bss[i].bssid[0], wl_bss[i].bssid[1], wl_bss[i].bssid[2],
               wl_bss[i].bssid[3], wl_bss[i].bssid[4], wl_bss[i].bssid[5],
               wl_bss[i].channel, wl_bss[i].freq, wl_bss[i].rssi, wl_bss[i].security,
               wl_bss[i].ssid);
    }
    CHECK(named >= 1);

    /* Which network the automatic join takes: none when nothing is configured, a
     * secured one refused but noted, an open one taken -- the strongest of its
     * name. */
    {
        u32 secured;

        CHECK(wl_config.loaded && wl_config.count == 0);
        CHECK(wl_pick(&secured) == -1 && !secured);
        setting_ssid = "net-axxxxxx";
        wl_load_config();
        CHECK(wl_pick(&secured) == -1 && secured);
        setting_ssid = "nowhere";
        wl_load_config();
        CHECK(wl_pick(&secured) == -1 && !secured);
        /* Several names, the strongest of any of them taken. */
        setting_ssid = " nowhere , net-axxxxxx ,net-cxxxxxxx";
        wl_load_config();
        CHECK(wl_config.count == 3 && wl_config.ssid_len[1] == 11);
        CHECK(!strcmp(wl_config.ssid[1], "net-axxxxxx"));
        CHECK(!strcmp(wl_config.ssid[2], "net-cxxxxxxx"));
        for (i = 0; i < wl_stats.bss_count && strcmp(wl_bss[i].ssid, "net-bx"); i++)
            ;
        CHECK(i < wl_stats.bss_count);
        if (i < wl_stats.bss_count) {
            u8 was = wl_bss[i].security;

            wl_bss[i].security = 0;
            setting_ssid = "net-bx";
            wl_load_config();
            CHECK(wl_pick(&secured) == (s32)i && !secured);
            wl_bss[i].security = was;
        }
        setting_ssid = "";
        wl_load_config();
    }
    report("wifi", outs, &st);
    printf("aic wifi: %u messages as Linux sent them, %u networks\n", outs_matched,
           wl_stats.bss_count);
}

/* --- the frames ------------------------------------------------------------------- */

static const u8 console_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
static const u8 sender_mac[6]  = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
static const u8 bssid_mac[6]   = { 0x68, 0x8f, 0xc9, 0x33, 0x7a, 0xc7 };

/* One received packet as the chip delivers it: the 60-byte hardware header whose
 * first 16 bits are the MPDU's length and whose byte 36 carries the decryption
 * status in bits 2-4, then an 802.11 from-DS data frame with its LLC/SNAP. */
static u32 build_rx(u8 *b, u32 qos, u32 decr, const u8 *payload, u32 payload_len)
{
    u32 hdr_len = qos ? 26 : 24, security = decr ? 8 : 0, at, i;
    u8 *mpdu = b + 60;

    memset(b, 0, 60);
    b[36] = (u8)(decr << 2);
    memset(mpdu, 0, 64);
    mpdu[0] = qos ? 0x88 : 0x08;            /* data, QoS data when asked */
    mpdu[1] = 0x02;                         /* from DS */
    for (i = 0; i < 6; i++) {
        mpdu[4 + i] = console_mac[i];       /* address 1: the destination */
        mpdu[10 + i] = bssid_mac[i];        /* address 2: the BSSID */
        mpdu[16 + i] = sender_mac[i];       /* address 3: the source */
    }
    at = hdr_len + security;
    mpdu[at] = 0xAA; mpdu[at + 1] = 0xAA; mpdu[at + 2] = 0x03;
    mpdu[at + 6] = 0x08; mpdu[at + 7] = 0x00;       /* ethertype IPv4 */
    at += 8;
    for (i = 0; i < payload_len; i++)
        mpdu[at + i] = payload[i];
    at += payload_len;
    b[0] = (u8)at;
    b[1] = (u8)(at >> 8);
    return 60 + at;
}

static void check_received(const char *what, const u8 *payload, u32 payload_len)
{
    u32 i, bad = 0;

    if (up_length != ETH_HLEN + payload_len) {
        printf("FAIL %s: length %u, wanted %u\n", what, up_length,
               ETH_HLEN + payload_len);
        failures++;
        return;
    }
    for (i = 0; i < 6; i++) {
        bad |= up_frame[i] != console_mac[i];
        bad |= up_frame[6 + i] != sender_mac[i];
    }
    bad |= up_frame[12] != 0x08 || up_frame[13] != 0x00;
    for (i = 0; i < payload_len; i++)
        bad |= up_frame[ETH_HLEN + i] != payload[i];
    if (bad) {
        printf("FAIL %s: header or payload wrong:", what);
        for (i = 0; i < 20 && i < up_length; i++)
            printf(" %02x", up_frame[i]);
        printf("\n");
        failures++;
    }
}

static void test_frames(void)
{
    static USB_DEV dev;
    static NET_DEV nd;
    static AIC a;
    static volatile AIC_STATS st;
    static u8 buffer[4096];
    static const u8 payload[] = { 0x45, 0x00, 0x00, 0x1c, 0xde, 0xad, 0xbe, 0xef };
    u8 frame[64];
    u32 n, i;

    wifi_phase = 1;
    dev.active = 1;
    nd.usb = &dev;
    nd.st = &net_counters;
    a.nd = &nd;
    a.dev = &dev;
    a.st = &st;
    a.vif = 0;
    a.ap_sta = 3;
    data_rx_count = 0;
    dma_used = 0;
    CHECK(aic_data_open(&a) == 0);
    a.joined = 1;

    /* Received: the plain case, where the Ethernet header being written overlaps
     * the addresses it is built from, and the QoS and encrypted case. */
    n = build_rx(buffer, 0, 0, payload, sizeof(payload));
    up_frames = 0;
    feed_rx(&a, buffer, n);
    CHECK(up_frames == 1);
    check_received("plain receive", payload, sizeof(payload));

    n = build_rx(buffer, 1, 3, payload, sizeof(payload));
    up_frames = 0;
    feed_rx(&a, buffer, n);
    CHECK(up_frames == 1);
    check_received("QoS encrypted receive", payload, sizeof(payload));
    CHECK(st.data_rx_frames == 2);
    CHECK(st.data_rx_short == 0 && st.data_rx_truncated == 0);
    CHECK(st.data_rx_not_from_ds == 0 && st.data_rx_unsupported == 0);

    /* A frame nobody can take apart yet is counted, not mangled. */
    n = build_rx(buffer, 1, 3, payload, sizeof(payload));
    buffer[48] = 0x01;                          /* an aggregated MSDU */
    up_frames = 0;
    feed_rx(&a, buffer, n);
    CHECK(up_frames == 0 && st.data_rx_unsupported == 1);

    /* A block ack's frames out of order, as the firmware hands up a retried one
     * after those behind it: held back and released in order once it comes. */
    {
        static const u32 seqs[] = { 100, 102, 103, 101, 104, 104, 99 };
        u8 p[sizeof(payload)];
        u32 k;

        up_frames = 0;
        memcpy(p, payload, sizeof(p));
        for (k = 0; k < sizeof(seqs) / sizeof(seqs[0]); k++) {
            p[4] = (u8)seqs[k];
            n = build_rx(buffer, 1, 3, p, sizeof(p));
            buffer[48] = 0x20;                  /* needs reordering */
            buffer[60 + 22] = (u8)(seqs[k] << 4);
            buffer[60 + 23] = (u8)(seqs[k] >> 4);
            feed_rx(&a, buffer, n);
            if (k == 2)
                CHECK(up_frames == 1 && st.reorder_held == 2);
        }
        CHECK(up_frames == 5);
        CHECK(up_order[0] == 100 && up_order[1] == 101 && up_order[2] == 102 &&
              up_order[3] == 103 && up_order[4] == 104);
        CHECK(st.reorder_late == 2);            /* 104 again, and 99 behind it */

        /* A hole nothing fills is given up on after a few worker ticks. */
        up_frames = 0;
        p[4] = 107;
        n = build_rx(buffer, 1, 3, p, sizeof(p));
        buffer[48] = 0x20;
        buffer[60 + 22] = (u8)(107 << 4);
        buffer[60 + 23] = (u8)(107 >> 4);
        feed_rx(&a, buffer, n);
        CHECK(up_frames == 0);
        queued_dpc = 0;
        for (k = 0; k < AIC_REORDER_TICKS; k++)
            aic_data_tick(&a);
        CHECK(queued_dpc != 0);
        if (queued_dpc)
            queued_dpc->DeferredRoutine(queued_dpc, queued_dpc->DeferredContext, 0, 0);
        CHECK(up_frames == 1 && up_order[0] == 107 && st.reorder_timeouts == 1);
        CHECK(a.reorder_free_count == AIC_REORDER_SLOTS);

        /* Joined again: the access point numbers from scratch, and so must we. */
        aic_data_rejoin(&a);
        up_frames = 0;
        p[4] = 3;
        n = build_rx(buffer, 1, 3, p, sizeof(p));
        buffer[48] = 0x20;
        buffer[60 + 22] = (u8)(3 << 4);
        buffer[60 + 23] = 0;
        feed_rx(&a, buffer, n);
        CHECK(up_frames == 1 && up_order[0] == 3);

        /* A sender that restarts without a new association is followed once it
         * has stayed behind the window long enough. */
        up_frames = 0;
        for (k = 0; k < 64; k++) {
            u32 sq = 3900 + k;              /* behind 4, the way sequence numbers wrap */

            p[4] = (u8)k;
            n = build_rx(buffer, 1, 3, p, sizeof(p));
            buffer[48] = 0x20;
            buffer[60 + 22] = (u8)(sq << 4);
            buffer[60 + 23] = (u8)(sq >> 4);
            feed_rx(&a, buffer, n);
        }
        CHECK(st.reorder_resyncs == 1);
        CHECK(up_frames == 64 - (AIC_REORDER_RESYNC - 1));
    }

    /* Sent: the four header bytes, then the descriptor, then the payload with the
     * Ethernet header taken off. */
    for (i = 0; i < 6; i++) {
        frame[i] = sender_mac[i];
        frame[6 + i] = console_mac[i];
    }
    frame[12] = 0x08;
    frame[13] = 0x00;
    for (i = 0; i < sizeof(payload); i++)
        frame[ETH_HLEN + i] = payload[i];
    sent_count = 0;
    CHECK(aic_data_xmit(&a, frame, ETH_HLEN + sizeof(payload)) == 1);
    CHECK(sent_count == 1);
    CHECK(sent_length == 4 + 28 + sizeof(payload));
    CHECK(sent[0] == 4 + 28 + sizeof(payload) && sent[1] == 0);
    CHECK(sent[2] == 0x01 && sent[3] == 0);
    /* The descriptor sits behind the four header bytes, so its own offsets -- the
     * payload length at 0, the addresses at 8 and 14, the ethertype at 20 -- all
     * land four bytes further into the transfer. */
    CHECK(sent[4] == sizeof(payload) && sent[5] == 0);     /* payload length */
    CHECK(sent[12] == sender_mac[0] && sent[17] == sender_mac[5]);
    CHECK(sent[18] == console_mac[0] && sent[23] == console_mac[5]);
    CHECK(sent[24] == 0x08 && sent[25] == 0x00);           /* ethertype, wire order */
    CHECK(sent[26] == AIC_AC_BE && sent[27] == 0);         /* best effort, TID 0 */
    CHECK(sent[28] == 0 && sent[29] == 3);                 /* interface, AP station */
    CHECK(sent[32] == payload[0] && sent[39] == payload[sizeof(payload) - 1]);

    /* No slot free: counted and refused, not queued twice. */
    for (i = 0; i < AIC_DATA_TX_COUNT; i++)
        a.data_tx_urb[i].Busy = 1;
    CHECK(aic_data_xmit(&a, frame, ETH_HLEN + sizeof(payload)) == 0);
    CHECK(st.data_tx_full == 1);
    CHECK(!aic_data_tx_space(&a));
    a.data_tx_urb[0].Busy = 0;
    CHECK(aic_data_tx_space(&a));

    if (!failures)
        printf("aic frames: receive and transmit as the chip wants them\n");
}

int main(void)
{
    char probe[512];
    FILE *f;

    test_wifi();
    test_frames();
    fw_dir = getenv("AIC_FW") ? getenv("AIC_FW") : "/lib/firmware/aic8800D80";
    snprintf(probe, sizeof(probe), "%s/fmacfw_8800d80_u02.bin", fw_dir);
    f = fopen(probe, "rb");
    if (f) {
        fclose(f);
        test_load();
    } else {
        printf("aic load test skipped: no firmware in %s\n", fw_dir);
    }
    if (failures)
        return 1;
    printf("all aic tests passed\n");
    return 0;
}
