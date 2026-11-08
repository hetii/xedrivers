/* The console's mixed sound as a device wants it. Pure code, tested on the host. */

#ifndef XE_AUDIO_PCM_H
#define XE_AUDIO_PCM_H

#include "kernel.h"

#define AUDIO_FRAME_SAMPLES 256u        /* a channel's samples in a frame: ~5.33 ms at 48 kHz */
#define AUDIO_CHANNELS      6u          /* planar, one channel after another */

/* The channels of a frame, in this order (XAudio's 5.1). */
#define AUDIO_FL  0u
#define AUDIO_FR  1u
#define AUDIO_C   2u
#define AUDIO_LFE 3u
#define AUDIO_SL  4u
#define AUDIO_SR  5u

/* The first two channels of a frame (6 x 256 floats) as interleaved s32 at full scale,
 * `volume` out of 256 applied. Clipped, never wrapped. Writes 2 x 256 values. */
void pcm_from_frame(const float *frame, s32 *stereo, u32 volume);

/* All of a frame mixed to two channels for a stereo card, as ITU-R BS.775:
 * L = FL + 0.707 C + 0.707 SL, R = FR + 0.707 C + 0.707 SR, LFE left out. A game's
 * dialogue is in C, which pcm_from_frame drops. Not scaled down: clipped instead, as the
 * mix is what the game made and -6 dB is the default volume. */
void pcm_stereo_downmix(const float *frame, s32 *stereo, u32 volume);

/* All six channels, interleaved in the frame's own order (which is USB Audio's for 5.1:
 * L, R, C, LFE, then the surrounds), `volume` applied. Writes 6 x 256 values. */
void pcm_six(const float *frame, s32 *six, u32 volume);

/* The stereo downmix in the first two of six slots, the other four silent: for a 5.1 card
 * while the console is set to stereo. */
void pcm_stereo_in_six(const float *frame, s32 *six, u32 volume);

/* `samples` values (frames x channels) to little-endian PCM of `bytes` per sample (2, 3 or
 * 4), what USB Audio wants. Writes samples x bytes. */
void pcm_to_packet(const s32 *values, u32 samples, u8 *out, u32 bytes);

#endif /* XE_AUDIO_PCM_H */
