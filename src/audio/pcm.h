/* The console's mixed sound as a device wants it. Pure code, tested on the host. */

#ifndef XE_AUDIO_PCM_H
#define XE_AUDIO_PCM_H

#include "kernel.h"

#define AUDIO_FRAME_SAMPLES 256u        /* a channel's samples in a frame: ~5.33 ms at 48 kHz */
#define AUDIO_CHANNELS      6u          /* planar, one channel after another */

/* The first two channels of a frame (6 x 256 floats) as interleaved s32 at full scale,
 * `volume` out of 256 applied. Clipped, never wrapped. Writes 2 x 256 values. */
void pcm_from_frame(const float *frame, s32 *stereo, u32 volume);

/* `frames` of that to little-endian PCM of `bytes` per sample (2, 3 or 4), what USB
 * Audio wants. Writes frames x 2 x bytes. */
void pcm_to_packet(const s32 *stereo, u32 frames, u8 *out, u32 bytes);

#endif /* XE_AUDIO_PCM_H */
