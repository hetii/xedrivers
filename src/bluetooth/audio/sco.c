/* See sco.h.
 *
 * In: the transport's bytes are SCO packets back to back (handle and flags, length, data);
 * one is put together, its samples doubled to 16 kHz by linear interpolation, and given to
 * the microphone. Out: the worker turns the console's mix (48 kHz stereo) into 8 kHz mono,
 * a sixth of it each averaged over its six, into a ring; the transport takes SCO packets of
 * it, header and all, as many bytes as each USB request carries.
 */

#include "sco.h"
#include "sbc.h"
#if XE_AUDIO
#include "audio.h"
#include "pcm.h"
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
static u32 out_at = 3 + SCO_PAYLOAD, out_payload = SCO_PAYLOAD;

/* mSBC (wideband): 60-byte units (the H2 header, a 57-byte frame, a pad byte) as a byte stream
 * across SCO packets of 24 bytes, which is what 9 bytes a millisecond carries (USB setting 1). */
#define H2_UNIT     60u
#define MSBC_BYTES  4096u               /* a power of two */
static u32 msbc;
static SBC enc, dec;
static u8 out_bytes[MSBC_BYTES];
static volatile u32 out_in, out_out;
static u8 in_bytes[2 * H2_UNIT];
static u32 in_bytes_have, h2_seq;

/* Units found at DISPATCH_LEVEL wait here for the worker: decoding is floating point, which a
 * DPC must not touch (the interrupted thread's registers are not saved: the console hung). */
#define UNITS 16u                       /* a power of two: 120 ms */
static u8 units[UNITS][57];
static volatile u32 units_in, units_out;
#if XE_AUDIO
static s16 frame_pcm[128];
static PCM_DECIMATOR decimator;
#endif

void sco_link(u32 h, u32 wideband)
{
    u32 i;

    msbc = wideband;
    sco_stats.wideband = wideband;
    out_payload = wideband ? SCO_MSBC_PAYLOAD : SCO_PAYLOAD;
    out_in = out_out = 0;
    in_bytes_have = 0;
    h2_seq = 0;
    units_in = units_out = 0;
    if (wideband) {
        sbc_init_msbc(&enc);
        for (i = 0; i < sizeof(dec); i++)
            ((u8 *)&dec)[i] = 0;
#if XE_AUDIO
        for (i = 0; i < sizeof(decimator); i++)
            ((u8 *)&decimator)[i] = 0;
#endif
    }
    handle = h;
    sco_stats.handle = h;
    in_have = 0;
    out_at = 3 + out_payload;
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

/* mSBC in: the payload bytes in a window, a unit decoded where an H2 header and the frame's
 * syncword are found; whatever does not line up is slid past. */
static void msbc_in(const u8 *p, u32 n)
{
    u32 i;

    for (i = 0; i < n; i++) {
        if (in_bytes_have == sizeof(in_bytes)) {
            u32 k;

            for (k = 1; k < in_bytes_have; k++)
                in_bytes[k - 1] = in_bytes[k];
            in_bytes_have--;
            sco_stats.bad_headers++;
        }
        in_bytes[in_bytes_have++] = p[i];
        while (in_bytes_have >= 3 && !(in_bytes[0] == 0x01 && (in_bytes[1] & 0x0F) == 0x08 &&
                                       in_bytes[2] == 0xAD)) {
            u32 k;

            for (k = 1; k < in_bytes_have; k++)
                in_bytes[k - 1] = in_bytes[k];
            in_bytes_have--;
        }
        if (in_bytes_have >= H2_UNIT) {
            u32 k;

            if (units_in - units_out < UNITS) {
                for (k = 0; k < 57; k++)
                    units[units_in & (UNITS - 1)][k] = in_bytes[2 + k];
                mem_sync();
                units_in++;
            } else {
                sco_stats.bad_frames++;
            }
            in_bytes_have = 0;
        }
    }
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
            if (msbc)
                msbc_in(in_packet + 3, in_packet[2]);
            else
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
        if (out_at == 3 + out_payload && msbc) {
            out_packet[0] = (u8)handle;
            out_packet[1] = (u8)((handle >> 8) & 0x0F);
            out_packet[2] = (u8)out_payload;
            for (k = 0; k < out_payload; k++) {
                if (out_in != out_out) {
                    out_packet[3 + k] = out_bytes[out_out & (MSBC_BYTES - 1)];
                    out_out++;
                } else {
                    out_packet[3 + k] = 0;
                    sco_stats.out_underruns++;
                }
            }
            sco_stats.packets_out++;
            out_at = 0;
        }
        if (out_at == 3 + out_payload) {
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

/* The worker's: the microphone's units decoded. */
static void msbc_decode_queued(void)
{
    static s16 pcm[128];

    while (units_out != units_in) {
        u32 got = sbc_decode(&dec, units[units_out & (UNITS - 1)], 57, pcm), k, peak = 0;

        mem_sync();
        units_out++;
        if (got != 120) {
            sco_stats.bad_frames++;
            continue;
        }
        for (k = 0; k < got; k++) {
            u32 a = pcm[k] < 0 ? (u32)-pcm[k] : (u32)pcm[k];

            if (a > peak)
                peak = a;
        }
        sco_stats.peak_in = peak;
        sco_stats.packets_in++;
        sco_stats.samples_in += got;
#if XE_AUDIO
        audio_mic_write(pcm, got);
#endif
    }
}

void sco_tick(void)
{
#if XE_AUDIO
    static s32 mix[96 * 2];
    u32 rounds;
#endif

    if (!handle)
        return;
    if (msbc)
        msbc_decode_queued();
#if XE_AUDIO
    if (msbc) {
        /* 360 frames of 48 kHz: 120 samples of 16 kHz, one mSBC frame, one 60-byte unit;
         * keep ~45 ms queued */
        static s32 mono[360];
        static const u8 seq[4] = { 0x08, 0x38, 0xC8, 0xF8 };

        for (rounds = 0; rounds < 16 && out_in - out_out < 6 * H2_UNIT; rounds++) {
            u32 i, made;
            u8 unit[H2_UNIT];

            audio_read(AUDIO_READER_VOICE, mix, 96);
            for (i = 0; i < 96; i++)
                mono[i] = ((mix[2 * i] >> 9) + (mix[2 * i + 1] >> 9));
            audio_read(AUDIO_READER_VOICE, mix, 96);
            for (i = 0; i < 96; i++)
                mono[96 + i] = ((mix[2 * i] >> 9) + (mix[2 * i + 1] >> 9));
            audio_read(AUDIO_READER_VOICE, mix, 96);
            for (i = 0; i < 96; i++)
                mono[192 + i] = ((mix[2 * i] >> 9) + (mix[2 * i + 1] >> 9));
            audio_read(AUDIO_READER_VOICE, mix, 72);
            for (i = 0; i < 72; i++)
                mono[288 + i] = ((mix[2 * i] >> 9) + (mix[2 * i + 1] >> 9));
            made = pcm_decimate3(&decimator, mono, 360, frame_pcm);
            if (made < 120)
                for (i = made; i < 120; i++)
                    frame_pcm[i] = 0;
            unit[0] = 0x01;
            unit[1] = seq[h2_seq++ & 3];
            sbc_encode(&enc, frame_pcm, unit + 2);
            unit[59] = 0;
            for (i = 0; i < H2_UNIT; i++) {
                out_bytes[out_in & (MSBC_BYTES - 1)] = unit[i];
                mem_sync();
                out_in++;
            }
            sco_stats.samples_out += 120;
        }
        return;
    }
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
