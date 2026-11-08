/* The microphone's 16 kHz mono, between a card's capture (drivers/uac/mic.c) and the console's
 * voice chat. A ring of samples: written from the USB completion, read by whoever gives them
 * to the chat. */

#include "audio.h"

#define MIC_RING 8192u                  /* samples, a power of two: 0.5 s */

volatile AUDIO_MIC_STATS audio_mic_stats;
static s16 mic_ring[MIC_RING];
static volatile u32 mic_write_at, mic_read_at;

void audio_mic_write(const s16 *samples, u32 count)
{
    u32 w = mic_write_at, i, peak = 0;

    if (w - mic_read_at + count > MIC_RING) {
        mic_read_at = w + count - MIC_RING;     /* nobody read: the oldest go */
        audio_mic_stats.overruns += count;
    }
    for (i = 0; i < count; i++) {
        s32 v = samples[i];

        mic_ring[(w + i) & (MIC_RING - 1)] = samples[i];
        if (v < 0)
            v = -v;
        if ((u32)v > peak)
            peak = (u32)v;
    }
    mem_sync();
    mic_write_at = w + count;
    audio_mic_stats.samples += count;
    audio_mic_stats.peak = peak;
    audio_mic_stats.ticks_alive++;
}

u32 audio_mic_read(s16 *samples, u32 count)
{
    u32 r = mic_read_at, have = mic_write_at - r, n = have < count ? have : count, i;

    for (i = 0; i < n; i++)
        samples[i] = mic_ring[(r + i) & (MIC_RING - 1)];
    mem_sync();
    mic_read_at = r + n;
    return n;
}

u32 audio_mic_ready(void)
{
    return mic_write_at - mic_read_at;
}
