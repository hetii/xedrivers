/* pcm.c on the host: channel choice, byte order, clipping, volume. */

#include <stdio.h>
#include <string.h>

#include "pcm.h"

static int failures;

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static float frame[AUDIO_CHANNELS * AUDIO_FRAME_SAMPLES];
static s32 stereo[AUDIO_FRAME_SAMPLES * 2];
static u8 out[16];

static int s16le(const u8 *p) { return (short)(p[0] | p[1] << 8); }

int main(void)
{
    frame[0] = 0.5f;                                 /* left 0 */
    frame[AUDIO_FRAME_SAMPLES] = -0.5f;              /* right 0 */
    frame[1] = 2.0f;                                 /* clipped high */
    frame[AUDIO_FRAME_SAMPLES + 1] = -2.0f;          /* clipped low */
    frame[2 * AUDIO_FRAME_SAMPLES] = 1.0f;           /* the third channel: not used */

    pcm_from_frame(frame, stereo, 256);
    CHECK(stereo[0] == 0x40000000);
    CHECK(stereo[1] == -0x40000000);
    CHECK(stereo[2] == 0x7FFFFFFF);
    CHECK(stereo[3] == (s32)0x80000000u);
    CHECK(stereo[4] == 0);

    pcm_to_packet(stereo, 2, out, 2);
    CHECK(s16le(out) == 16384 && s16le(out + 2) == -16384);
    CHECK(s16le(out + 4) == 32767 && s16le(out + 6) == -32768);

    pcm_from_frame(frame, stereo, 128);              /* half volume */
    CHECK(stereo[0] == 0x20000000);

    pcm_from_frame(frame, stereo, 256);
    pcm_to_packet(stereo, 1, out, 3);                /* 24 bits, little-endian */
    CHECK(out[0] == 0x00 && out[1] == 0x00 && out[2] == 0x40);
    CHECK(out[3] == 0x00 && out[4] == 0x00 && out[5] == 0xC0);

    pcm_to_packet(stereo, 1, out, 4);
    CHECK(out[3] == 0x40 && out[7] == 0xC0);

    if (failures)
        return 1;
    printf("all pcm tests passed\n");
    return 0;
}
