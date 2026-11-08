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

void pcm_stereo_downmix(const float *frame, s32 *stereo, u32 volume)
{
    const float k = 0.70710678f;
    const float *fl = frame + AUDIO_FL * AUDIO_FRAME_SAMPLES, *fr = frame + AUDIO_FR * AUDIO_FRAME_SAMPLES;
    const float *c = frame + AUDIO_C * AUDIO_FRAME_SAMPLES;
    const float *sl = frame + AUDIO_SL * AUDIO_FRAME_SAMPLES, *sr = frame + AUDIO_SR * AUDIO_FRAME_SAMPLES;
    u32 i;

    for (i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
        float centre = k * c[i];

        stereo[i * 2] = scale(fl[i] + centre + k * sl[i], volume);
        stereo[i * 2 + 1] = scale(fr[i] + centre + k * sr[i], volume);
    }
}

void pcm_six(const float *frame, s32 *six, u32 volume)
{
    u32 i, c;

    for (i = 0; i < AUDIO_FRAME_SAMPLES; i++)
        for (c = 0; c < 6; c++)
            six[i * 6 + c] = scale(frame[c * AUDIO_FRAME_SAMPLES + i], volume);
}

void pcm_stereo_in_six(const float *frame, s32 *six, u32 volume)
{
    s32 two[2 * AUDIO_FRAME_SAMPLES];
    u32 i, c;

    pcm_stereo_downmix(frame, two, volume);
    for (i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
        six[i * 6] = two[i * 2];
        six[i * 6 + 1] = two[i * 2 + 1];
        for (c = 2; c < 6; c++)
            six[i * 6 + c] = 0;
    }
}

void pcm_to_packet(const s32 *values, u32 samples, u8 *out, u32 bytes)
{
    u32 i;

    for (i = 0; i < samples; i++) {
        u32 v = (u32)values[i];

        /* the top `bytes` of the value, low byte first */
        if (bytes == 4)
            *out++ = (u8)v;
        if (bytes >= 3)
            *out++ = (u8)(v >> 8);
        *out++ = (u8)(v >> 16);
        *out++ = (u8)(v >> 24);
    }
}
