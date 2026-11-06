/* The console's sound for whoever plays it: a USB audio card today, a Bluetooth link
 * later. The kernel hands every mixed frame to a registered client (XAudio's MEC
 * exports, 800-802); capture.c keeps them, stereo, in a ring a player reads from.
 *
 * Registered at the first audio_open and for good: with nobody reading the callback
 * returns at once, so a console with no audio device plugged in never registers and
 * one whose device left costs a function call every 5.33 ms.
 */

#ifndef XE_AUDIO_H
#define XE_AUDIO_H

#include "kernel.h"

/* Worker thread, PASSIVE_LEVEL: start hearing the console; 0, or what failed. */
u32  audio_open(void);
/* Any IRQL: nobody is reading any more. */
void audio_close(void);

/* `frames` of stereo into `stereo` (2 x s32 at full scale, left first), whatever there
 * is: the rest is silence and is counted. From the player's completion, DISPATCH_LEVEL.
 * Nothing is given until a little has gathered, so that the first frames do not run dry. */
void audio_read(s32 *stereo, u32 frames);

typedef struct {
    u32 open_status;        /* of the registration */
    u32 readers;
    u32 callbacks;
    u32 capture_errors;
    u32 frames_in;
    u32 frames_out;
    u32 overruns;           /* frames lost: nobody read them in time */
    u32 underruns;          /* frames of silence given: none there */
    u32 volume;             /* out of 256, from audio_volume (0-100) in the ini */
    u32 callback_context;   /* processor << 8 | IRQL of the kernel's call */
} AUDIO_STATS;

extern volatile AUDIO_STATS audio_stats;

#endif /* XE_AUDIO_H */
