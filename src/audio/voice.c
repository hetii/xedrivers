/* The microphone's 16 kHz mono, between a card's capture (drivers/uac/mic.c) and the console's
 * voice chat, as usb-audio-x360 does it (TimeoutTim/usb-audio-x360, driver.cpp, audio.cpp):
 *
 *  - three xam exports are entered through ours first (detour.c): XamVoiceHeadsetPresent (781)
 *    says yes while the card's microphone runs; XamVoiceSubmitPacket (782) takes the title's
 *    microphone requests (direction 1) and fills them from the ring, from the capture's own
 *    completion; XamVoiceGetBatteryStatus (784) says full for our headset;
 *  - player 0's voice route (xam's binding category 1, the one a wireless headset's quadrant
 *    button switches) is pointed at a virtual standalone headset, the route it had kept to be
 *    put back when the microphone goes. Through xam's own binding routine, at 0x816D9060 on
 *    17559, checked instruction by instruction before use, and its table at 0x81A82FF4.
 *
 * A request (XHV's) is words: [0] status, [1] length done, [2] buffer, [3] units (two 16-bit
 * samples each), [5] format. It is answered as 16 kHz PCM, format 0, with [3] and [1] the bytes:
 * left at format 1 (G.726) XHV would decode our PCM as G.726.
 */

#include "audio.h"
#include "detour.h"
#include "thunk.h"
#include "usb.h"

#define MIC_RING 8192u                  /* samples, a power of two: 0.5 s */

volatile AUDIO_MIC_STATS audio_mic_stats;
static s16 mic_ring[MIC_RING];
static volatile u32 mic_write_at, mic_read_at;

void audio_mic_write(const s16 *samples, u32 count)
{
    u32 w = mic_write_at, i, peak = 0;

    if (w - mic_read_at + count > MIC_RING) {
        mic_read_at = w + count - MIC_RING;     /* nobody read: the oldest go */
        audio_mic_stats.overruns += count;
    }
    for (i = 0; i < count; i++) {
        s32 v = samples[i];

        mic_ring[(w + i) & (MIC_RING - 1)] = samples[i];
        if (v < 0)
            v = -v;
        if ((u32)v > peak)
            peak = (u32)v;
    }
    mem_sync();
    mic_write_at = w + count;
    audio_mic_stats.samples += count;
    audio_mic_stats.peak = peak;
    audio_mic_stats.ticks_alive++;
}

u32 audio_mic_read(s16 *samples, u32 count)
{
    u32 r = mic_read_at, have = mic_write_at - r, n = have < count ? have : count, i;

    for (i = 0; i < n; i++)
        samples[i] = mic_ring[(r + i) & (MIC_RING - 1)];
    mem_sync();
    mic_read_at = r + n;
    return n;
}

u32 audio_mic_ready(void)
{
    return mic_write_at - mic_read_at;
}

/* --- the voice chat ----------------------------------------------------------------------- */

#define PACKET_SLOTS        64u
#define PACKET_PENDING      0x103u
#define PACKET_FAILED       0xC0000001u
#define MIC_FRESH_SAMPLES   1600u       /* the microphone counts as there while it has written
                                         * within this many samples' worth of ticks (0.1 s) */
#define BIND_WRAPPER        0x816D9060u
#define BIND_TABLE_POINTER  0x81A82FF4u
#define VOICE_CATEGORY      1u
#define VIRTUAL_DEVICE      0xA7554D49u
#define VIRTUAL_CONTEXT     0x40000000u
#define BIND_EVERY_TICKS    60u         /* ~2 s of worker ticks */

extern int XamVoiceHeadsetPresent(void *handle);
extern int XamVoiceSubmitPacket(void *handle, u32 direction, void *packet);
extern int XamVoiceGetBatteryStatus(u32 user, u32 *status);
extern int XamUserGetDeviceContext(u32 user, u32 category, u32 *context);
extern int XamUserGetSigninState(u32 user);

volatile AUDIO_VOICE_STATS audio_voice_stats;
static DETOUR present_detour, submit_detour, battery_detour;
static u32 bind_checked, bind_ok;
static volatile u32 bound, bound_user = 0xFF;
static u32 displaced_context, displaced_device, bind_ticks, last_alive, quiet_ticks;
#define QUIET_TICKS 15u                 /* ~0.5 s of worker ticks with nothing captured: gone */

typedef struct {
    volatile u32 state;                 /* 0 free, 1 taken, 2 queued, 3 being answered */
    volatile u32 *words;
    u32 units;
} SLOT;

static SLOT slots[PACKET_SLOTS];

/* Whether the microphone runs: written to within the last few worker ticks. */
static u32 mic_alive(void)
{
    return audio_voice_stats.alive;
}

static u32 call_original(DETOUR *d, u32 a, u32 b, u32 c)
{
    return usb_framed_call((void *)d->code, a, b, c);
}

/* Answer what is queued, oldest slot first, while the ring has a request's worth. Called from
 * the capture's completion (DISPATCH_LEVEL) and from the worker. */
void audio_voice_answer(void)
{
    u32 i;

    for (i = 0; i < PACKET_SLOTS; i++) {
        SLOT *s = &slots[i];
        u32 samples;

        if (s->state != 2)
            continue;
        samples = s->units * 2;
        if (audio_mic_ready() < samples)
            return;                     /* in order: nothing after it either */
        if (!__sync_bool_compare_and_swap(&s->state, 2u, 3u))
            continue;
        audio_mic_read((s16 *)(unsigned long)s->words[2], samples);
        s->words[3] = s->units * 4;
        s->words[5] = 0;
        s->words[1] = s->words[3];
        mem_sync();
        s->words[0] = 0;
        audio_voice_stats.answered++;
        s->words = 0;
        mem_sync();
        s->state = 0;
    }
}

static void fail_all(void)
{
    u32 i;

    for (i = 0; i < PACKET_SLOTS; i++) {
        SLOT *s = &slots[i];

        if (!__sync_bool_compare_and_swap(&s->state, 2u, 3u))
            continue;
        s->words[1] = 0;
        s->words[5] = 0;
        mem_sync();
        s->words[0] = PACKET_FAILED;
        audio_voice_stats.failed++;
        s->words = 0;
        mem_sync();
        s->state = 0;
    }
}

USB_ENTERED int voice_present_hook(void *handle)
{
    if (call_original(&present_detour, (u32)(unsigned long)handle, 0, 0))
        return 1;
    return mic_alive();
}

USB_ENTERED int voice_submit_hook(void *handle, u32 direction, void *packet)
{
    volatile u32 *words = (volatile u32 *)packet;
    u32 i;

    if (direction == 1 && words && mic_alive() &&
        (bound || !call_original(&present_detour, (u32)(unsigned long)handle, 0, 0)) &&
        words[2] && words[3] && words[3] <= 512) {
        for (i = 0; i < PACKET_SLOTS; i++) {
            SLOT *s = &slots[i];

            if (!__sync_bool_compare_and_swap(&s->state, 0u, 1u))
                continue;
            s->words = words;
            s->units = words[3];
            words[0] = PACKET_PENDING;
            mem_sync();
            s->state = 2;
            audio_voice_stats.queued++;
            return 0;
        }
        audio_voice_stats.full++;
    }
    return (int)call_original(&submit_detour, (u32)(unsigned long)handle, direction,
                              (u32)(unsigned long)packet);
}

USB_ENTERED int voice_battery_hook(u32 user, u32 *status)
{
    if (status && bound && user == bound_user) {
        *status = 3;                    /* full: it runs on USB power */
        return 0;
    }
    return (int)call_original(&battery_detour, user, (u32)(unsigned long)status, 0);
}

#if defined(__powerpc__)
USB_ENTRY(voice_present_entry, voice_present_hook);
USB_ENTRY(voice_submit_entry, voice_submit_hook);
USB_ENTRY(voice_battery_entry, voice_battery_hook);
extern char voice_present_entry[], voice_submit_entry[], voice_battery_entry[];
#endif

static int bind(u32 device, u32 context, u32 remove, u8 *user)
{
    return (int)usb_framed_call((void *)(unsigned long)BIND_WRAPPER, device, context,
                                VOICE_CATEGORY, remove, (u32)(unsigned long)user);
}

static u32 bind_routine_is_ours(void)
{
    static const u32 expected[] = {
        0x7C8B2378u, 0x7CA42B78u, 0x54CA063Fu, 0x41820010u, 0x7CE53B78u, 0x7D635B78u,
        0x4BFFFED8u, 0x7C852378u, 0x7CE63B78u, 0x7D645B78u, 0x4BFFFC50u,
    };
    const volatile u32 *code = (const volatile u32 *)(unsigned long)BIND_WRAPPER;
    u32 i;

    if (!MmIsAddressValid((const void *)code))
        return 0;
    for (i = 0; i < sizeof(expected) / sizeof(expected[0]); i++)
        if (code[i] != expected[i])
            return 0;
    return 1;
}

/* Player 0's voice record: 0x40 bytes a player, category c's (active, context) at 8 + 8c. */
static volatile u32 *player0_record(void)
{
    u32 table = *(volatile u32 *)(unsigned long)BIND_TABLE_POINTER;

    if (!table || (table & 3u) || table < 0x80000000u || table >= 0xA0000000u)
        return 0;
    return (volatile u32 *)(unsigned long)table;
}

static void put_back(void)
{
    u8 user = 0;

    if (!displaced_context)
        return;
    bind(displaced_device, displaced_context, 0, &user);
    displaced_context = 0;
    displaced_device = 0;
}

static void unbind(void)
{
    u8 user = 0xFF;

    if (!bound)
        return;
    bind(VIRTUAL_DEVICE, VIRTUAL_CONTEXT, 1, &user);
    bound = 0;
    bound_user = 0xFF;
    audio_voice_stats.bound = 0;
    put_back();
}

/* Make room in player 0's voice route: whatever it has is taken off, kept to be put back. */
static u32 free_player0(void)
{
    u32 context = 0, family;
    volatile u32 *r;
    u8 removed = 0xFF;

    if ((s32)usb_framed_call((void *)XamUserGetDeviceContext, 0, VOICE_CATEGORY,
                             (u32)(unsigned long)&context) < 0 || !context ||
        context == VIRTUAL_CONTEXT)
        return 1;
    r = player0_record();
    if (!r || !r[2 + 2 * VOICE_CATEGORY] || r[3 + 2 * VOICE_CATEGORY] != context)
        return 0;
    family = context & 0xF0000000u;
    displaced_context = context;
    displaced_device = family == 0x40000000u || family == 0x50000000u ? r[1] : r[0];
    if (bind(displaced_device, context, 1, &removed) < 0 || removed != 0) {
        displaced_context = 0;
        displaced_device = 0;
        return 0;
    }
    return 1;
}

static void try_bind(void)
{
    u8 user = 0;

    if (bound || !bind_ok)
        return;
    audio_voice_stats.signin = usb_framed_call((void *)XamUserGetSigninState, 0);
    if (!audio_voice_stats.signin) {
        audio_voice_stats.bind_step = 1;    /* nobody signed in on player 0 */
        return;
    }
    if (!free_player0()) {
        audio_voice_stats.bind_step = 2;
        return;
    }
    audio_voice_stats.bind_status = (u32)bind(VIRTUAL_DEVICE, VIRTUAL_CONTEXT, 0, &user);
    if ((s32)audio_voice_stats.bind_status < 0) {
        audio_voice_stats.bind_step = 3;
        put_back();
        return;
    }
    audio_voice_stats.bind_user = user;
    if (user != 0) {                    /* xam chose another player: not ours to take */
        audio_voice_stats.bind_step = 4;
        u8 removed = 0xFF;

        bind(VIRTUAL_DEVICE, VIRTUAL_CONTEXT, 1, &removed);
        put_back();
        return;
    }
    audio_voice_stats.bind_step = 0;
    bound_user = 0;
    bound = 1;
    audio_voice_stats.bound = 1;
    audio_voice_stats.binds++;
}

void audio_voice_init(void)
{
#if defined(__powerpc__)
    /* The symbols are our own import thunks: the functions are where those lead. */
    void *present = (void *)(unsigned long)thunk_reads((u32)(unsigned long)XamVoiceHeadsetPresent);
    void *submit = (void *)(unsigned long)thunk_reads((u32)(unsigned long)XamVoiceSubmitPacket);
    void *battery = (void *)(unsigned long)thunk_reads((u32)(unsigned long)XamVoiceGetBatteryStatus);

    if (!present || !submit || !battery)
        return;
    audio_voice_stats.hooks =
        detour_install(&present_detour, present, voice_present_entry) |
        detour_install(&submit_detour, submit, voice_submit_entry) << 1 |
        detour_install(&battery_detour, battery, voice_battery_entry) << 2;
#endif
}

/* Every worker tick (PASSIVE_LEVEL). */
void audio_voice_tick(void)
{
    u32 written = audio_mic_stats.samples;

    /* A tick can come twice between two of the capture's 4 ms transfers: only a run of quiet
     * ticks means the microphone went. */
    quiet_ticks = written != last_alive ? 0 : quiet_ticks + 1;
    last_alive = written;
    audio_voice_stats.alive = quiet_ticks < QUIET_TICKS;
    if (!audio_voice_stats.alive) {
        fail_all();
        if (bound)
            unbind();
        return;
    }
    audio_voice_answer();
    if (!bind_checked) {
        bind_checked = 1;
        bind_ok = bind_routine_is_ours();
        audio_voice_stats.bind_routine = bind_ok;
    }
    if (!bound && ++bind_ticks >= BIND_EVERY_TICKS) {
        bind_ticks = 0;
        try_bind();
    }
}
