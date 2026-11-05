/* How many frames go into each USB packet. Pure code, tested on the host.
 *
 * A synchronous or adaptive device takes 48 a millisecond. An asynchronous one runs on
 * its own clock and says how fast through its feedback endpoint: samples per frame as a
 * fixed-point number whose format differs by device (10.14 in three bytes at full
 * speed, 16.16 in four), brought to 16.16 by shifting until it lies near 48, the way
 * Linux's snd-usb-audio does.
 */

#ifndef XE_UAC_PACER_H
#define XE_UAC_PACER_H

#include "kernel.h"

#define PACER_NOMINAL (48u << 16)

typedef struct {
    u32 rate;               /* samples a millisecond, 16.16 */
    u32 phase;
    u32 most;               /* frames a packet may carry */
    u32 asynchronous;
} UAC_PACER;

static inline void pacer_begin(UAC_PACER *p, u32 asynchronous, u32 most)
{
    p->rate = PACER_NOMINAL;
    p->phase = 0;
    p->most = most;
    p->asynchronous = asynchronous;
}

/* The next packet's frames. */
static inline u32 pacer_next(UAC_PACER *p)
{
    u32 total, frames;

    if (!p->asynchronous)
        return 48;
    total = p->phase + p->rate;
    frames = total >> 16;
    if (frames > p->most)
        frames = p->most;
    p->phase = total & 0xFFFFu;
    return frames;
}

/* A value from the feedback endpoint (`bytes` of it, little-endian); 1 if taken. */
static inline u32 pacer_feedback(UAC_PACER *p, const u8 *data, u32 bytes)
{
    u32 raw, i;

    if (!p->asynchronous || bytes < 3 || bytes > 4)
        return 0;
    raw = 0;
    for (i = 0; i < bytes; i++)
        raw |= (u32)data[i] << (8 * i);
    if (!raw)
        return 0;
    while (raw < PACER_NOMINAL - PACER_NOMINAL / 4)
        raw <<= 1;
    while (raw > PACER_NOMINAL + PACER_NOMINAL / 2)
        raw >>= 1;
    if (raw < (47u << 16) || raw > (p->most << 16))
        return 0;
    p->rate = raw;
    return 1;
}

#endif /* XE_UAC_PACER_H */
