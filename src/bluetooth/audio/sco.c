/* See sco.h.
 *
 * In: the transport's bytes are SCO packets back to back (handle and flags, length, data);
 * one is put together, its samples doubled to 16 kHz by linear interpolation, and given to
 * the microphone. Out: the worker turns the console's mix (48 kHz stereo) into 8 kHz mono,
 * a sixth of it each averaged over its six, into a ring; the transport takes SCO packets of
 * it, header and all, as many bytes as each USB request carries.
 */

#include "sco.h"
#if XE_AUDIO
#include "audio.h"
#endif

#define RING        2048u           /* samples, a power of two: 256 ms at 8 kHz */
#define START       256u            /* gathered before the first goes: 32 ms */

volatile SCO_STATS sco_stats;

static volatile u32 handle;
static s16 ring[RING];
static volatile u32 ring_in, ring_out, started;

/* in: one packet being put together */
static u8 in_packet[3 + 255];
static u32 in_have;
static s16 in_last;

/* out: where in the current packet the transport is */
static u8 out_packet[3 + SCO_PAYLOAD];
static u32 out_at = sizeof(out_packet);

void sco_link(u32 h)
{
    handle = h;
    sco_stats.handle = h;
    in_have = 0;
    out_at = sizeof(out_packet);
    ring_out = ring_in;
    started = 0;
#if XE_AUDIO
    if (h)
        audio_open(AUDIO_READER_VOICE, AUDIO_STEREO);
    else
        audio_close(AUDIO_READER_VOICE);
#endif
}

void sco_up(const s16 *in, u32 n, s16 *out, s16 *last)
{
    u32 i;
    s32 prev = *last;

    for (i = 0; i < n; i++) {
        out[2 * i] = (s16)((prev + in[i]) / 2);
        out[2 * i + 1] = in[i];
        prev = in[i];
    }
    *last = (s16)prev;
}

static void packet_in(const u8 *p, u32 n)
{
    s16 pcm[127], up[254];
    u32 samples = n / 2, i, peak = 0;

    if (samples > 127)
        samples = 127;
    for (i = 0; i < samples; i++) {
        s16 v = (s16)(p[2 * i] | p[2 * i + 1] << 8);
        u32 a = v < 0 ? (u32)-v : (u32)v;

        pcm[i] = v;
        if (a > peak)
            peak = a;
    }
    sco_stats.peak_in = peak;
    sco_stats.packets_in++;
    sco_stats.samples_in += samples;
    sco_up(pcm, samples, up, &in_last);
#if XE_AUDIO
    audio_mic_write(up, samples * 2);
#endif
}

void bt_sco_take(const u8 *d, u32 n)
{
    u32 i;

    for (i = 0; i < n; i++) {
        in_packet[in_have++] = d[i];
        if (in_have == 2 && ((u32)(in_packet[0] | (in_packet[1] & 0x0F) << 8) != handle)) {
            /* not a header of ours: slide on by one byte */
            sco_stats.bad_headers++;
            in_packet[0] = in_packet[1];
            in_have = 1;
            continue;
        }
        if (in_have >= 3 && in_have == 3u + in_packet[2]) {
            packet_in(in_packet + 3, in_packet[2]);
            in_have = 0;
        }
        if (in_have >= sizeof(in_packet))
            in_have = 0;
    }
}

void bt_sco_fill(u8 *d, u32 n)
{
    u32 i, k;

    for (i = 0; i < n; i++) {
        if (out_at == sizeof(out_packet)) {
            u32 have = ring_in - ring_out;

            out_packet[0] = (u8)handle;
            out_packet[1] = (u8)((handle >> 8) & 0x0F);
            out_packet[2] = SCO_PAYLOAD;
            if (!started && have >= START)
                started = 1;
            for (k = 0; k < SCO_PAYLOAD / 2; k++) {
                s16 v = 0;

                if (started && ring_in != ring_out) {
                    v = ring[ring_out & (RING - 1)];
                    ring_out++;
                } else {
                    sco_stats.out_underruns++;
                    started = 0;
                }
                out_packet[3 + 2 * k] = (u8)v;
                out_packet[4 + 2 * k] = (u8)((u16)v >> 8);
            }
            sco_stats.packets_out++;
            sco_stats.samples_out += SCO_PAYLOAD / 2;
            out_at = 0;
        }
        d[i] = out_packet[out_at++];
    }
}

void sco_tick(void)
{
#if XE_AUDIO
    static s32 mix[96 * 2];
    u32 rounds;

    if (!handle)
        return;
    /* the console gives 48 kHz: 96 frames a round, 16 samples out; keep ~64 ms in the ring,
     * whatever the tick (20 ms is 160 samples: 10 rounds) */
    for (rounds = 0; rounds < 64 && ring_in - ring_out < 512; rounds++) {
        u32 i, k;

        audio_read(AUDIO_READER_VOICE, mix, 96);
        for (i = 0; i < 16; i++) {
            s32 sum = 0;

            for (k = 0; k < 6; k++)
                sum += (mix[(i * 6 + k) * 2] >> 16) + (mix[(i * 6 + k) * 2 + 1] >> 16);
            if (ring_in - ring_out >= RING) {
                sco_stats.out_overruns++;
                continue;
            }
            ring[ring_in & (RING - 1)] = (s16)(sum / 12);
            mem_sync();
            ring_in++;
        }
    }
#endif
}
