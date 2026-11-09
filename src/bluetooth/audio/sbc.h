/* SBC, the codec every A2DP sink takes (A2DP 1.4, Appendix B): an encoder with 8 subbands,
 * any block count, every channel mode and both allocations, so the same code makes standard
 * SBC (bitpool 53) and SBC-XQ (dual channel, bitpool 76 and up). Pure; tests/bluetooth.
 */

#ifndef XE_SBC_H
#define XE_SBC_H

#include "kernel.h"

#define SBC_RATE_16000  0u
#define SBC_RATE_32000  1u
#define SBC_RATE_44100  2u
#define SBC_RATE_48000  3u

#define SBC_MONO        0u
#define SBC_DUAL        1u
#define SBC_STEREO      2u
#define SBC_JOINT       3u

#define SBC_LOUDNESS    0u
#define SBC_SNR         1u

#define SBC_SUBBANDS    8u
#define SBC_FRAME_MAX   520u            /* 2 channels, 16 blocks, bitpool 250 */

typedef struct {
    u32 rate;                           /* SBC_RATE_* */
    u32 blocks;                         /* 4, 8, 12 or 16 */
    u32 mode;                           /* SBC_MONO .. SBC_JOINT */
    u32 allocation;
    u32 bitpool;                        /* 2..250 (mono, dual: up to 16 x subbands) */
    u32 msbc;                           /* mSBC's frame: header 0xAD 0 0, the rest fixed */
    float x[2][80];                     /* the analysis filter's history, per channel */
    float v[2][160];                    /* the synthesis filter's, for decoding */
} SBC;

void sbc_init(SBC *s, u32 rate, u32 blocks, u32 mode, u32 allocation, u32 bitpool);

/* Samples one frame takes, per channel: blocks x 8. */
u32  sbc_samples(const SBC *s);

/* Bytes one frame takes. */
u32  sbc_frame_length(const SBC *s);

/* One frame of `pcm` (interleaved, one or two channels as the mode says, sbc_samples each)
 * into `out`; returns its length. */
u32  sbc_encode(SBC *s, const s16 *pcm, u8 *out);

/* mSBC (HFP's wideband voice): 16 kHz mono, 15 blocks, loudness, bitpool 26; 57-byte frames
 * of 120 samples. */
void sbc_init_msbc(SBC *s);

/* One frame from `in` (`n` bytes) into `pcm` (interleaved); the parameters are the frame's
 * own, written into `s`. Returns the samples per channel, 0 for a frame that does not parse
 * or whose CRC is wrong. */
u32  sbc_decode(SBC *s, const u8 *in, u32 n, s16 *pcm);

#endif /* XE_SBC_H */
