/* The kernel's mixed frames into a ring; see audio.h. The callback comes from the audio
 * driver's own thread and only copies and converts: no waiting, no USB, no file.
 *
 * XAudioRegisterRenderDriverMECClient (800) takes { callback, context } and gives a
 * handle; XAudioCaptureRenderDriverFrame (802) fills 6 x 256 floats from it inside the
 * callback. Known from usb-audio-x360 and X360AudioStream, both on retail 17559.
 */

#include "audio.h"
#include "file.h"
#include "pcm.h"
#include "usb.h"

#define RING_FRAMES     4096u           /* a power of two: ~85 ms */
#define START_FRAMES    1024u           /* ~21 ms gathered before the first frame goes */
#define DEFAULT_VOLUME  50u             /* -6 dB: a USB card at 100 played louder than HDMI */
#define POOL_TAG        0x6F647561u     /* "audo" */

typedef struct {
    void (*callback)(void *context);
    void *context;
} MEC_CLIENT;

extern s32 XAudioRegisterRenderDriverMECClient(MEC_CLIENT *client, void **handle);
extern s32 XAudioCaptureRenderDriverFrame(void *handle, float *samples);
/* Kernel 511: the dashboard's speaker setting. The digital part, above 16 bits. */
extern s32 XAudioGetSpeakerConfig(u32 *config);
#define SPEAKERS_DIGITAL 0xFFFF0000u
extern void *ExAllocatePoolTypeWithTag(u32 bytes, u32 tag, u32 pool_type);

volatile AUDIO_STATS audio_stats;

static MEC_CLIENT client;
static void *handle;
static float frame[AUDIO_CHANNELS * AUDIO_FRAME_SAMPLES] __attribute__((aligned(128)));
static s32 converted[AUDIO_FRAME_SAMPLES * 6];
static s32 *ring;
static volatile u32 write_at, read_at;      /* in frames, running on; one writer, one reader */
static volatile u32 reading, started;
static u32 channels = 2;                   /* a ring frame's values */

void audio_callback(void *context)
{
    u32 w, i, used;

    (void)context;
    audio_stats.callback_context = ke_processor() << 8 | ke_irql();
    if (!reading || !handle)
        return;
    audio_stats.callbacks++;
    if (XAudioCaptureRenderDriverFrame(handle, frame) < 0) {
        audio_stats.capture_errors++;
        return;
    }
    w = write_at;
    used = w - read_at;
    if (used + AUDIO_FRAME_SAMPLES > RING_FRAMES) {
        audio_stats.overruns += AUDIO_FRAME_SAMPLES;
        return;                             /* the reader moves read_at: this frame goes */
    }
    if (audio_stats.layout == AUDIO_SIX)
        pcm_six(frame, converted, audio_stats.volume);
    else if (audio_stats.layout == AUDIO_STEREO_IN_SIX)
        pcm_stereo_in_six(frame, converted, audio_stats.volume);
    else
        pcm_stereo_downmix(frame, converted, audio_stats.volume);
    for (i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
        u32 at = ((w + i) & (RING_FRAMES - 1)) * channels, c;

        for (c = 0; c < channels; c++)
            ring[at + c] = converted[i * channels + c];
    }
    mem_sync();
    write_at = w + AUDIO_FRAME_SAMPLES;
    audio_stats.frames_in += AUDIO_FRAME_SAMPLES;
}

USB_STUB("audio_entry", "lis 12, audio_callback@ha\n    addi 12, 12, audio_callback@l");
void audio_entry(void *context);

static u32 volume_setting(void)
{
    char value[8];
    u32 n = file_setting("xedrivers.ini", "audio_volume", value, sizeof(value)), i, v = 0;

    if (!n)
        return DEFAULT_VOLUME;
    for (i = 0; i < n && value[i] >= '0' && value[i] <= '9'; i++)
        v = v * 10 + (u32)(value[i] - '0');
    return v > 100 ? 100 : v;
}

u32 audio_surround(void)
{
    u32 speakers = 0;

    audio_stats.speaker_status = (u32)XAudioGetSpeakerConfig(&speakers);
    audio_stats.speaker_config = speakers;
    return (s32)audio_stats.speaker_status >= 0 && (speakers & SPEAKERS_DIGITAL) != 0;
}

u32 audio_open(u32 layout)
{
    reading = 0;                            /* the callback leaves the ring alone */
    mem_sync();
    audio_stats.volume = volume_setting() * 256u / 100u;
    audio_stats.layout = layout;
    channels = layout == AUDIO_STEREO ? 2 : 6;
    if (!ring) {
        /* Room for six channels whichever card comes first. */
        ring = (s32 *)ExAllocatePoolTypeWithTag(RING_FRAMES * 6 * sizeof(s32), POOL_TAG, 2);
        if (!ring)
            return 1;
    }
    read_at = write_at;
    started = 0;
    mem_sync();
    if (!handle) {
        void *h = 0;
        s32 status;

        client.callback = audio_entry;
        client.context = 0;
        status = XAudioRegisterRenderDriverMECClient(&client, &h);
        audio_stats.open_status = (u32)status;
        if (status < 0 || !h)
            return 2;
        handle = h;
    }
    reading = 1;
    audio_stats.readers = 1;
    return 0;
}

void audio_close(void)
{
    reading = 0;
    audio_stats.readers = 0;
}

void audio_read(s32 *out, u32 frames)
{
    u32 r = read_at, have = write_at - r, n, i, c;

    if (!started) {
        if (have < START_FRAMES)
            have = 0;
        else
            started = 1;
    }
    n = have < frames ? have : frames;
    for (i = 0; i < n; i++) {
        u32 at = ((r + i) & (RING_FRAMES - 1)) * channels;

        for (c = 0; c < channels; c++)
            out[i * channels + c] = ring[at + c];
    }
    for (; i < frames; i++)
        for (c = 0; c < channels; c++)
            out[i * channels + c] = 0;
    mem_sync();
    read_at = r + n;
    audio_stats.frames_out += n;
    audio_stats.underruns += frames - n;
    if (n < frames)
        started = 0;                        /* gather again rather than crackle */
}
