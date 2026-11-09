/* The card's microphone: an isochronous IN stream beside the output, brought to 16 kHz mono,
 * what the console's voice chat takes (XHV, through audio_mic_write). Same transfers as the
 * output (stream.h): 4 ms each, full speed a frame a packet, high speed a microframe a
 * packet with only every period-th asked for. The chat's format is fixed, 16 kHz mono 16-bit;
 * up to it nothing is thrown away: the card's best setting, all its channels, 24 bits through
 * a proper low-pass. Two transfers out at once; each completion
 * takes what came and asks again.
 *
 * Packets come back at the offsets their asked-for lengths give, each with its own actual
 * length; a short one is no error (OHCI's data underrun, 0xC0050003).
 */

#include "stream.h"
#include "audio.h"
#include "pcm.h"

#define STATUS_UNDERRUN     0xC0050003u
#define BAD_IN_A_ROW        16u

#define MIC_STEP_NO_RATE    2u
#define MIC_STEP_INTERFACE  3u
#define MIC_STEP_ENDPOINT   4u
#define MIC_STEP_BUFFER     5u
#define MIC_STEP_FORMAT     6u

/* One packet's frames: the channels averaged, each sample at 24-bit scale (a 16-bit card's
 * moved up), then 48 to 16 kHz through the decimator (pcm.c). */
static void take(UAC *u, const u8 *data, u32 length)
{
    const UAC_STREAM *m = &u->info.mic;
    u32 frame = m->channels * m->bytes, n = length / frame, i, c;
    s32 mono[512];                          /* a packet: at most 1023 bytes, 511 frames */
    s16 out[512 / 3 + 2];                   /* on the DPC's stack: keep it small */
    u32 made;

    if (n > sizeof(mono) / sizeof(mono[0]))
        n = sizeof(mono) / sizeof(mono[0]);
    for (i = 0; i < n; i++) {
        s32 sum = 0;

        for (c = 0; c < m->channels; c++) {
            const u8 *s = data + i * frame + c * m->bytes + m->bytes - 1;   /* the top byte */
            s32 v = (s32)((u32)s[0] << 24 | (u32)s[-1] << 16 | (m->bytes > 2 ? (u32)s[-2] << 8 : 0));

            sum += v >> 8;                  /* 24-bit scale: room for 8 channels */
        }
        mono[i] = sum / (s32)m->channels;
    }
    made = pcm_decimate3(&u->mic_filter, mono, n, out);
    if (made) {
        audio_mic_write(out, made);
        u->st->mic_samples += made;
        audio_voice_answer();
    }
}

static void mic_ask(UAC *u, u32 slot)
{
    u32 i;

    for (i = 0; i < u->packets; i++)
        u->mic_lengths[slot][i] = i % u->period != u->period - 1 ? 0 : (u16)u->mic_room;
    usb_isoch(u->dev, &u->mic[slot], u->mic_lengths[slot]);
}

static void mic_done(ISOCH *t, u32 *statuses, u16 *lengths)
{
    UAC *u = (UAC *)t->Owner;
    u32 slot = (u32)(t - u->mic), i, at = 0, bad = 0;
    const u8 *base = u->mic_buf + slot * UAC_MIC_BUFFER;

    u->st->mic_transfers++;
    for (i = 0; statuses && lengths && i < u->packets; i++) {
        u32 asked = u->mic_lengths[slot][i];

        if (asked) {
            if (statuses[i] == 0 || statuses[i] == STATUS_UNDERRUN) {
                u->st->mic_packets_ok++;
                if (lengths[i] && lengths[i] <= asked)
                    take(u, base + at, lengths[i]);
            } else {
                u->st->mic_packets_bad++;
                u->st->mic_last_status = statuses[i];
                bad++;
            }
        }
        at += asked;
    }
    if (u->stopping)
        return;
    if (!statuses || bad == u->packets / u->period) {
        if (++u->mic_bad_in_a_row >= BAD_IN_A_ROW)
            return;                         /* the microphone gives up, the output plays on */
    } else {
        u->mic_bad_in_a_row = 0;
    }
    mic_ask(u, slot);
}

/* The sample rate on the microphone's endpoint (UAC1); a UAC2 card shares the output's clock,
 * set already. */
static u32 mic_rate(UAC *u)
{
    u32 actual = 0;

    if (u->info.version != 1)
        return 0;
    u->control[0] = (u8)UAC_RATE;
    u->control[1] = (u8)(UAC_RATE >> 8);
    u->control[2] = (u8)(UAC_RATE >> 16);
    uac_control(u, 0x22, 0x01, 0x0100, u->info.mic.endpoint, 3, &actual);
    return 0;
}

void mic_start(UAC *u)
{
    const UAC_STREAM *m = &u->info.mic;
    u32 slot, need;

    u->mic_bad_in_a_row = 0;
    {
        u8 *p = (u8 *)&u->mic_filter;
        u32 k;

        for (k = 0; k < sizeof(u->mic_filter); k++)
            p[k] = 0;
    }
    if (m->bytes < 2 || m->channels < 1) {
        u->st->mic_step = MIC_STEP_FORMAT;
        return;
    }
    /* A data packet: a period's frames and one more, never past the endpoint's largest. */
    u->mic_room = (u->frames_nominal + 1) * m->channels * m->bytes;
    if (u->mic_room > m->max_packet)
        u->mic_room = m->max_packet;
    need = (u->packets / u->period) * u->mic_room;
    if (need > UAC_MIC_BUFFER) {
        u->st->mic_step = MIC_STEP_BUFFER;
        return;
    }
    if (!NT_SUCCESS(uac_control(u, 0x01, 0x0B, m->alternate, m->interface, 0, 0))) {
        u->st->mic_step = MIC_STEP_INTERFACE;
        return;
    }
    if (mic_rate(u)) {
        u->st->mic_step = MIC_STEP_NO_RATE;
        return;
    }
    u->ep_mic = usb_open(u->dev, USB_TYPE_ISOCH, m->endpoint, m->max_packet,
                         u->packets == UAC_HS_PACKETS ? 1 : m->interval);
    if (!u->ep_mic) {
        u->st->mic_step = MIC_STEP_ENDPOINT;
        return;
    }
    if (!u->mic_buf) {
        u->mic_buf = (u8 *)MmAllocatePhysicalMemory(2, UAC_MIC_SLOTS * UAC_MIC_BUFFER, 4);
        if (!u->mic_buf) {
            u->st->mic_step = MIC_STEP_BUFFER;
            return;
        }
    }
    for (slot = 0; slot < UAC_MIC_SLOTS; slot++) {
        ISOCH *t = &u->mic[slot];

        t->Context = u->ep_mic;
        t->Done = mic_done;
        t->Endpoint = u->ep_mic;
        t->Buffer = u->mic_buf + slot * UAC_MIC_BUFFER;
        t->Length = need;
        t->PacketCount = (u8)u->packets;
        t->Owner = u;
        mic_ask(u, slot);
    }
    u->st->mic_step = 0;
}
