/* Native test of the AIC8800D80 firmware load (aic_fw.c over aic_cmd.c) against
 * Linux's own: tests/drivers/aic8800/load-8d80.txt is aic_load_fw's load of the same
 * firmware, recorded with usbmon. Every message the driver sends must be the one
 * Linux sent, byte for byte (by length and CRC-32, the recording keeps no firmware),
 * and the chip's answers are replayed from the recording.
 *
 * Needs the firmware files (AIC_FW, /lib/firmware/aic8800D80 by default); without
 * them the test says so and passes. Run by `make test`. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aic.h"
#include "file.h"

#define EP_IN  ((void *)1)
#define EP_OUT ((void *)2)

volatile FILE_STATS file_stats;
volatile USB_STATS usb_stats;

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                      failures++; } } while (0)

/* --- the recording --------------------------------------------------------------- */

typedef struct { int out; u32 length; u32 crc; u8 *data; } STEP;
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

    if (!f)
        return 0;
    while (fgets(line, sizeof(line), f) && step_count < 2048) {
        STEP *s = &steps[step_count];

        if (!strncmp(line, "out ", 4)) {
            s->out = 1;
            sscanf(line + 4, "%u %x", &s->length, &s->crc);
            step_count++;
        } else if (!strncmp(line, "in ", 3)) {
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

static URB *rx_waiting[AIC_RX_COUNT];
static u32 rx_waiting_count;

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
        else if (first_mismatch == 0xFFFFFFFFu)
            first_mismatch = outs_matched;
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

/* --- the test ------------------------------------------------------------------- */

int main(void)
{
    static USB_DEV dev;
    static NET_DEV nd;
    static AIC a;
    static volatile AIC_STATS st;
    char probe[512];
    FILE *f;
    u32 step, outs = 0, i;

    fw_dir = getenv("AIC_FW") ? getenv("AIC_FW") : "/lib/firmware/aic8800D80";
    snprintf(probe, sizeof(probe), "%s/fmacfw_8800d80_u02.bin", fw_dir);
    f = fopen(probe, "rb");
    if (!f) {
        printf("aic tests skipped: no firmware in %s\n", fw_dir);
        return 0;
    }
    fclose(f);
    if (!load_recording("tests/drivers/aic8800/load-8d80.txt")) {
        printf("FAIL: no recording\n");
        return 1;
    }
    for (i = 0; i < step_count; i++)
        outs += steps[i].out;

    dev.active = 1;
    nd.usb = &dev;
    a.nd = &nd;
    a.dev = &dev;
    a.st = &st;
    CHECK(aic_open(&a) == 0);
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
    if (outs_matched != outs)
        printf("  matched %u of %u messages, first difference at %u (step %u/%#x)\n",
               outs_matched, outs, first_mismatch, st.step, st.step_status);

    if (failures)
        return 1;
    printf("all aic tests passed (%u messages as Linux sent them)\n", outs_matched);
    return 0;
}
