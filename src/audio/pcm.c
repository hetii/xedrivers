/* See pcm.h. */

#include "pcm.h"

static s32 scale(float v, u32 volume)
{
    float x = v * (float)volume / 256.0f;

    if (x >= 1.0f)
        return 0x7FFFFFFF;
    if (x <= -1.0f)
        return (s32)0x80000000u;
    return (s32)(x * 2147483648.0f);
}

void pcm_from_frame(const float *frame, s32 *stereo, u32 volume)
{
    const float *right = frame + AUDIO_FRAME_SAMPLES;
    u32 i;

    for (i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
        stereo[i * 2] = scale(frame[i], volume);
        stereo[i * 2 + 1] = scale(right[i], volume);
    }
}

void pcm_to_packet(const s32 *stereo, u32 frames, u8 *out, u32 bytes)
{
    u32 i;

    for (i = 0; i < frames * 2; i++) {
        u32 v = (u32)stereo[i];

        /* the top `bytes` of the value, low byte first */
        if (bytes == 4)
            *out++ = (u8)v;
        if (bytes >= 3)
            *out++ = (u8)(v >> 8);
        *out++ = (u8)(v >> 16);
        *out++ = (u8)(v >> 24);
    }
}
