/* How many frames go into each USB packet. Pure code, tested on the host.
 *
 * A synchronous or adaptive device takes the nominal count each packet: 48 a millisecond
 * at full speed, 6 a microframe at high speed (times the interval). An asynchronous one runs on
 * its own clock and says how fast through its feedback endpoint: samples per frame as a
 * fixed-point number whose format differs by device (10.14 in three bytes at full
 * speed, 16.16 in four), brought to 16.16 by shifting until it lies near 48, the way
 * Linux's snd-usb-audio does.
 */

#ifndef XE_UAC_PACER_H
#define XE_UAC_PACER_H

#include "kernel.h"

typedef struct {
    u32 nominal;            /* samples a packet, 16.16 */
    u32 rate;               /* samples a packet, 16.16: the nominal, or what the device said */
    u32 phase;
    u32 most;               /* frames a packet may carry */
    u32 asynchronous;
} UAC_PACER;

static inline void pacer_begin(UAC_PACER *p, u32 asynchronous, u32 frames, u32 most)
{
    p->nominal = frames << 16;
    p->rate = p->nominal;
    p->phase = 0;
    p->most = most;
    p->asynchronous = asynchronous;
}

/* The next packet's frames. */
static inline u32 pacer_next(UAC_PACER *p)
{
    u32 total, frames;

    if (!p->asynchronous)
        return p->nominal >> 16;
    total = p->phase + p->rate;
    frames = total >> 16;
    if (frames > p->most)
        frames = p->most;
    p->phase = total & 0xFFFFu;
    return frames;
}

/* A value from the feedback endpoint (`bytes` of it, little-endian); 1 if taken. A
 * high-speed device counts per microframe: the shifts below bring it to a packet's worth,
 * the interval being a power of two. */
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
    while (raw < p->nominal - p->nominal / 4)
        raw <<= 1;
    while (raw > p->nominal + p->nominal / 2)
        raw >>= 1;
    if (raw < p->nominal - p->nominal / 32 || raw > (p->most << 16))
        return 0;
    p->rate = raw;
    return 1;
}

#endif /* XE_UAC_PACER_H */
