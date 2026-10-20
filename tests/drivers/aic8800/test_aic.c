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

#define EP_IN  ((void *)1)
#define EP_OUT ((void *)2)

volatile FILE_STATS file_stats;
volatile USB_STATS usb_stats;

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                      failures++; } } while (0)

/* --- the recording --------------------------------------------------------------- */

typedef struct { int out; u32 length; u32 crc; u8 *data; } STEP;
static URB *rx_waiting[AIC_RX_COUNT];
static u32 rx_waiting_count;
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

void usb_submit(URB *urb)
{
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

void *usb_open(USB_DEV *dev, u32 type, u32 address, u32 max_packet, u32 interval)
{
    (void)dev; (void)type; (void)max_packet; (void)interval;
    return address & 0x80 ? EP_IN : EP_OUT;
}

void usb_wait(u32 ms) { (void)ms; }
void usb_wake(void) { }
void usb_sleep(u32 ms) { (void)ms; }

static u8 dma[64 * 1024];
static u32 dma_used;

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
    dev.active = 1;
    nd.usb = &dev;
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
    dev.active = 1;
    nd.usb = &dev;
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
    report("wifi", outs, &st);
    printf("aic wifi: %u messages as Linux sent them, %u networks\n", outs_matched,
           wl_stats.bss_count);
}

int main(void)
{
    char probe[512];
    FILE *f;

    test_wifi();
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
