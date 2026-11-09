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
#include "bench.h"

#define RING_FRAMES     16384u          /* a power of two: ~341 ms */
#define START_FRAMES    1024u           /* ~21 ms gathered before the first frame goes */

/* What a reader gathers first, and keeps when it fell a ring behind. A card reads from its
 * transfers' completions every few ms; the Bluetooth readers from the worker, which can be
 * 100 ms late now and then: with 85 ms of ring and 21 ms gathered, a headset heard 285 ms of
 * silence and lost 317 ms every 10 s (027570, 2026-11-10). */
static const u32 start_frames[AUDIO_READERS] = { START_FRAMES, 6144u, 4096u };
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
static s32 stereo_frame[AUDIO_FRAME_SAMPLES * 2];
static s32 six_frame[AUDIO_FRAME_SAMPLES * 6];

/* Two rings filled from every frame -- the stereo mix and the six channels as they are --
 * so that readers of either layout can play at once (a USB card and a Bluetooth headset).
 * One writer; each reader has its own place, and one that falls a ring behind skips ahead
 * itself: the writer never waits for anybody. */
static s32 *stereo_ring, *six_ring;
static volatile u32 write_at;               /* in frames, running on */

typedef struct {
    volatile u32 active;
    u32 layout;
    volatile u32 read_at;
    u32 started;
} READER;

static READER readers[AUDIO_READERS];
static volatile u32 reading;                /* any reader */

void audio_callback(void *context)
{
    u32 w, i, c;

    (void)context;
    audio_stats.callback_context = ke_processor() << 8 | ke_irql();
    /* The one clock the dim does not stop: wake the worker and the Bluetooth host, whose
     * timed waits it leaves unended (usb.c). Through a DPC, whatever this IRQL is. */
    usb_wake_soon();
    if (!reading || !handle)
        return;
    audio_stats.callbacks++;
    if (XAudioCaptureRenderDriverFrame(handle, frame) < 0) {
        audio_stats.capture_errors++;
        return;
    }
    bench_audio_frame(frame);
    w = write_at;
    pcm_stereo_downmix(frame, stereo_frame, audio_stats.volume);
    pcm_six(frame, six_frame, audio_stats.volume);
    for (i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
        u32 at = (w + i) & (RING_FRAMES - 1);

        stereo_ring[at * 2] = stereo_frame[i * 2];
        stereo_ring[at * 2 + 1] = stereo_frame[i * 2 + 1];
        for (c = 0; c < 6; c++)
            six_ring[at * 6 + c] = six_frame[i * 6 + c];
    }
    mem_sync();
    write_at = w + AUDIO_FRAME_SAMPLES;
    audio_stats.frames_in += AUDIO_FRAME_SAMPLES;
}

USB_STUB("audio_entry", "lis 12, audio_callback@ha\n    addi 12, 12, audio_callback@l");
void audio_entry(void *context);

/* Read once: every (re)start of a stream used to read the file, a Bluetooth stream's step
 * change among them -- file I/O on the USB medium in the middle of streaming. */
static u32 volume_setting(void)
{
    static u32 cached = 0xFFFFFFFFu;
    char value[8];
    u32 n, i, v = 0;

    if (cached != 0xFFFFFFFFu)
        return cached;
    n = file_setting("xedrivers.ini", "audio_volume", value, sizeof(value));
    if (!n)
        return cached = DEFAULT_VOLUME;
    for (i = 0; i < n && value[i] >= '0' && value[i] <= '9'; i++)
        v = v * 10 + (u32)(value[i] - '0');
    return cached = v > 100 ? 100 : v;
}

u32 audio_surround(void)
{
    u32 speakers = 0;

    audio_stats.speaker_status = (u32)XAudioGetSpeakerConfig(&speakers);
    audio_stats.speaker_config = speakers;
    return (s32)audio_stats.speaker_status >= 0 && (speakers & SPEAKERS_DIGITAL) != 0;
}

u32 audio_open(u32 reader, u32 layout)
{
    READER *r = &readers[reader];
    u32 i, any = 0;

    if (reader >= AUDIO_READERS)
        return 3;
    r->active = 0;
    mem_sync();
    audio_stats.volume = volume_setting() * 256u / 100u;
    audio_stats.layout = layout;
    if (!stereo_ring) {
        stereo_ring = (s32 *)ExAllocatePoolTypeWithTag(RING_FRAMES * 8 * sizeof(s32),
                                                       POOL_TAG, 2);
        if (!stereo_ring)
            return 1;
        six_ring = stereo_ring + RING_FRAMES * 2;
    }
    r->layout = layout;
    r->read_at = write_at;
    r->started = 0;
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
    r->active = 1;
    reading = 1;
    for (i = 0; i < AUDIO_READERS; i++)
        any += readers[i].active;
    audio_stats.readers = any;
    return 0;
}

u32 audio_frames_played(void)
{
    return write_at;
}

void audio_close(u32 reader)
{
    u32 i, any = 0;

    if (reader >= AUDIO_READERS)
        return;
    readers[reader].active = 0;
    for (i = 0; i < AUDIO_READERS; i++)
        any += readers[i].active;
    reading = any != 0;
    audio_stats.readers = any;
}

void audio_read(u32 reader, s32 *out, u32 frames)
{
    READER *rd = &readers[reader];
    u32 w = write_at, r = rd->read_at, have, n, i, c;
    u32 channels = rd->layout == AUDIO_STEREO ? 2 : 6;

    if (!stereo_ring || !rd->active) {          /* no ring (its pool refused it) or not open */
        for (i = 0; i < frames * channels; i++)
            out[i] = 0;
        audio_stats.underruns += frames;
        return;
    }
    if (w - r > RING_FRAMES - AUDIO_FRAME_SAMPLES) {    /* a ring behind: skip ahead */
        audio_stats.overruns += w - r - start_frames[reader];
        r = w - start_frames[reader];
    }
    have = w - r;
    if (!rd->started) {
        if (have < start_frames[reader])
            have = 0;
        else
            rd->started = 1;
    }
    n = have < frames ? have : frames;
    for (i = 0; i < n; i++) {
        u32 at = (r + i) & (RING_FRAMES - 1);

        if (rd->layout == AUDIO_STEREO) {
            out[i * 2] = stereo_ring[at * 2];
            out[i * 2 + 1] = stereo_ring[at * 2 + 1];
        } else if (rd->layout == AUDIO_SIX) {
            for (c = 0; c < 6; c++)
                out[i * 6 + c] = six_ring[at * 6 + c];
        } else {                                        /* the stereo mix in front, the rest silent */
            out[i * 6] = stereo_ring[at * 2];
            out[i * 6 + 1] = stereo_ring[at * 2 + 1];
            for (c = 2; c < 6; c++)
                out[i * 6 + c] = 0;
        }
    }
    for (; i < frames; i++)
        for (c = 0; c < channels; c++)
            out[i * channels + c] = 0;
    mem_sync();
    rd->read_at = r + n;
    audio_stats.frames_out += n;
    audio_stats.underruns += frames - n;
    if (n < frames)
        rd->started = 0;                    /* gather again rather than crackle */
}
