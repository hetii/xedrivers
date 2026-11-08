/* descriptors.c on the host, with configuration descriptors as devices send them. */

#include <stdio.h>
#include <string.h>

#include "descriptors.h"
#include "pacer.h"

static int failures;

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* A UAC1 adapter (as C-Media's): stereo out at 44.1 and 48 kHz, 16 bits, adaptive; mono
 * mic at 48 kHz; a zero-bandwidth setting before each. */
static const u8 uac1[] = {
    9, 2, 0, 0, 3, 1, 0, 0x80, 50,
    9, 4, 0, 0, 0, 1, 1, 0, 0,
    9, 0x24, 1, 0x00, 0x01, 40, 0, 2, 1,                 /* AC header 1.00 */
    /* speaker stream */
    9, 4, 1, 0, 0, 1, 2, 0, 0,
    9, 4, 1, 1, 1, 1, 2, 0, 0,
    7, 0x24, 1, 1, 1, 1, 0,                              /* AS general: PCM */
    14, 0x24, 2, 1, 2, 2, 16, 2, 0x44, 0xAC, 0, 0x80, 0xBB, 0,
    9, 5, 0x01, 0x09, 200, 0, 1, 0, 0,
    7, 0x25, 1, 1, 1, 1, 0,
    /* mic stream */
    9, 4, 2, 0, 0, 1, 2, 0, 0,
    9, 4, 2, 1, 1, 1, 2, 0, 0,
    7, 0x24, 1, 2, 1, 1, 0,
    11, 0x24, 2, 1, 1, 2, 16, 1, 0x80, 0xBB, 0,
    9, 5, 0x82, 0x05, 100, 0, 1, 0, 0,
    7, 0x25, 1, 1, 0, 0, 0,
};

/* UAC1 with two sample sizes and asynchronous feedback through bSynchAddress. */
static const u8 uac1_async[] = {
    9, 4, 0, 0, 0, 1, 1, 0, 0,
    9, 0x24, 1, 0x00, 0x01, 40, 0, 1, 1,
    9, 4, 1, 1, 2, 1, 2, 0, 0,
    7, 0x24, 1, 1, 1, 1, 0,
    11, 0x24, 2, 1, 2, 3, 24, 1, 0x80, 0xBB, 0,
    9, 5, 0x01, 0x05, 0x26, 1, 1, 0, 0x81,
    9, 5, 0x81, 0x11, 3, 0, 1, 1, 0,
    9, 4, 1, 2, 2, 1, 2, 0, 0,
    7, 0x24, 1, 1, 1, 1, 0,
    11, 0x24, 2, 1, 2, 2, 16, 1, 0x80, 0xBB, 0,
    9, 5, 0x01, 0x05, 200, 0, 1, 0, 0x81,
    9, 5, 0x81, 0x11, 3, 0, 1, 1, 0,
};

/* UAC2: clock source, 24 bits in 4-byte slots, asynchronous with a feedback endpoint. */
static const u8 uac2[] = {
    9, 4, 0, 0, 0, 1, 1, 0x20, 0,
    9, 0x24, 1, 0x00, 0x02, 8, 40, 0, 0,
    8, 0x24, 0x0A, 5, 3, 7, 0, 0,                        /* clock source 5 */
    9, 4, 1, 1, 2, 1, 2, 0x20, 0,
    16, 0x24, 1, 2, 0, 1, 1, 0, 0, 0, 2, 3, 0, 0, 0, 0,  /* PCM, 2 channels */
    6, 0x24, 2, 1, 4, 24,
    7, 5, 0x03, 0x05, 0x88, 1, 1,
    7, 5, 0x83, 0x11, 4, 0, 1,
};

/* A surround-only output: 5.1, taken as play6. */
static const u8 surround[] = {
    9, 4, 0, 0, 0, 1, 1, 0, 0,
    9, 0x24, 1, 0x00, 0x01, 40, 0, 1, 1,
    9, 4, 1, 1, 1, 1, 2, 0, 0,
    7, 0x24, 1, 1, 1, 1, 0,
    11, 0x24, 2, 1, 6, 2, 16, 1, 0x80, 0xBB, 0,
    9, 5, 0x01, 0x09, 0x4C, 2, 1, 0, 0,                  /* 588: 49 frames of 6 x 2 bytes */
};

/* Stops mid-descriptor: read as far as it goes, no further. */
static const u8 cut[] = { 9, 4, 0, 0, 0, 1, 1, 0, 0, 9, 0x24, 1 };

int main(void)
{
    UAC_INFO i;

    CHECK(uac_parse(uac1, sizeof(uac1), &i));
    CHECK(i.version == 1 && i.control_interface == 0);
    CHECK(i.play.found && i.play.interface == 1 && i.play.alternate == 1);
    CHECK(i.play.channels == 2 && i.play.bytes == 2 && i.play.bits == 16);
    CHECK(i.play.endpoint == 0x01 && i.play.max_packet == 200);
    CHECK(i.play.sync == UAC_SYNC_ADAPTIVE && !i.play.feedback && i.play.rate_listed);
    CHECK(i.mic.found && i.mic.interface == 2 && i.mic.channels == 1 && i.mic.endpoint == 0x82);

    CHECK(uac_parse(uac1_async, sizeof(uac1_async), &i));
    CHECK(i.play.alternate == 2 && i.play.bytes == 2);       /* 16 bits preferred */
    CHECK(i.play.sync == UAC_SYNC_ASYNC);
    CHECK(i.play.feedback == 0x81 && i.play.feedback_max_packet == 3);
    CHECK(!i.mic.found);                                     /* the feedback is no microphone */

    CHECK(uac_parse(uac2, sizeof(uac2), &i));
    CHECK(i.version == 2 && i.clock == 5);
    CHECK(i.play.found && i.play.channels == 2 && i.play.bytes == 4 && i.play.bits == 24);
    CHECK(i.play.sync == UAC_SYNC_ASYNC && i.play.feedback == 0x83);
    CHECK(i.play.terminal == 2);

    CHECK(uac_parse(surround, sizeof(surround), &i));
    CHECK(!i.play.found && i.play6.found && i.play6.channels == 6 && i.play6.max_packet == 588);
    CHECK(!uac_parse(cut, sizeof(cut), &i));
    CHECK(!uac_parse(cut, 0, &i));
    CHECK(uac_packet_bytes(&(UAC_STREAM){ .channels = 2, .bytes = 2 }) == 196);

    {
        UAC_PACER p;
        u32 k, sum = 0;
        static const u8 q10_14[] = { 0x00, 0x00, 0x0C };      /* 48.0 in 10.14, three bytes */
        static const u8 q16_16[] = { 0x00, 0x80, 0x30, 0x00 }; /* 48.5 in 16.16 */
        static const u8 wild[] = { 0xFF, 0xFF, 0xFF, 0x7F };

        pacer_begin(&p, 0, 49);
        CHECK(pacer_next(&p) == 48);
        pacer_begin(&p, 1, 49);
        CHECK(pacer_next(&p) == 48);
        CHECK(pacer_feedback(&p, q10_14, 3) && p.rate == 48u << 16);
        CHECK(pacer_feedback(&p, q16_16, 4) && p.rate == (48u << 16 | 0x8000u));
        for (k = 0; k < 1000; k++)
            sum += pacer_next(&p);
        CHECK(sum == 48500);                                  /* the clock followed exactly */
        CHECK(!pacer_feedback(&p, wild, 4) || p.rate <= 49u << 16);
        CHECK(!pacer_feedback(&p, q10_14, 2));
    }

    if (failures)
        return 1;
    printf("all uac descriptor tests passed\n");
    return 0;
}
