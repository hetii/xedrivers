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

/* What a card is sent. */
#define AUDIO_STEREO        2u      /* 5.1 mixed down to two channels */
#define AUDIO_SIX           6u      /* the six channels as they are */
#define AUDIO_STEREO_IN_SIX 7u      /* the stereo mix in a six-channel stream, the rest silent */

/* Who reads: each has its own place in the rings, so they play at once. */
#define AUDIO_READER_CARD      0u   /* a USB card (uac) */
#define AUDIO_READER_BLUETOOTH 1u   /* an A2DP headset */
#define AUDIO_READER_VOICE     2u   /* a hands-free headset's earpiece (SCO) */
#define AUDIO_READERS          3u

/* Worker thread, PASSIVE_LEVEL: start hearing the console, as `reader`, in one of the
 * layouts above; 0, or what failed. */
u32  audio_open(u32 reader, u32 layout);

/* Worker thread: whether the dashboard's audio setting asks for 5.1 (Dolby Digital, with
 * or without WMA Pro), read from the kernel each call. Checked on 17559: 0x00000003 for
 * digital stereo, 0x00010001 for Dolby Digital 5.1, 0x00030001 with WMA Pro. */
u32  audio_surround(void);
/* Any IRQL: `reader` reads no more. */
void audio_close(u32 reader);

/* `frames` into `out` (channels x s32 at full scale, as the layout opened), whatever there
 * is: the rest is silence and is counted. From the player's completion, DISPATCH_LEVEL.
 * Nothing is given until a little has gathered, so that the first frames do not run dry. */
void audio_read(u32 reader, s32 *out, u32 frames);

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
    u32 speaker_status;     /* XAudioGetSpeakerConfig, last call: what it returned */
    u32 speaker_config;     /* and the word it gave */
    u32 layout;             /* AUDIO_* in use */
} AUDIO_STATS;

extern volatile AUDIO_STATS audio_stats;

/* The microphone (voice.c): 16 kHz mono, written by a card's capture (any IRQL up to
 * DISPATCH), read by the voice chat. */
void audio_mic_write(const s16 *samples, u32 count);
u32  audio_mic_read(s16 *samples, u32 count);      /* how many it gave */
u32  audio_mic_ready(void);

typedef struct {
    u32 samples;
    u32 overruns;           /* samples dropped: nobody read them */
    u32 peak;               /* of the last write, out of 32767 */
    u32 ticks_alive;        /* writes */
} AUDIO_MIC_STATS;

extern volatile AUDIO_MIC_STATS audio_mic_stats;

/* The console's voice chat (voice.c). init once, early, from the worker; tick every worker
 * tick; answer whenever the microphone has written (the capture's completion does). */
void audio_voice_init(void);
void audio_voice_tick(void);
void audio_voice_answer(void);

typedef struct {
    u32 hooks;              /* bits: 0 present, 1 submit, 2 battery hooked */
    u32 bind_routine;       /* xam's binding routine is the one checked */
    u32 alive;              /* the microphone wrote since the last tick */
    u32 bound;              /* player 0's voice route is ours */
    u32 binds;
    u32 queued;             /* microphone requests taken */
    u32 answered;
    u32 failed;
    u32 full;               /* requests passed on: no slot free */
    u32 bind_step;          /* why not bound: 1 not signed in, 2 route not freed, 3 bind failed,
                             * 4 given to another player */
    u32 signin;             /* XamUserGetSigninState(0) */
    u32 bind_status;
    u32 bind_user;
} AUDIO_VOICE_STATS;

extern volatile AUDIO_VOICE_STATS audio_voice_stats;

#endif /* XE_AUDIO_H */
