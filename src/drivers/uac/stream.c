/* The isochronous transfers: sound out, an asynchronous device's clock in. Each
 * completion refills its own transfer and queues it again from the USB DPC, so the
 * stream keeps itself going with nothing else to wake.
 *
 * Status words, as the kernel's OHCI side gives them (usb-audio-x360's audit, retail
 * 17559): 0 good, 0xC005100E not reached in time (late, nothing lost but that packet),
 * 0xC0050003 data underrun, which on an IN packet is only a short one.
 */

#include "stream.h"
#include "audio.h"
#include "pcm.h"

#define STATUS_LATE         0xC005100Eu
#define STATUS_UNDERRUN     0xC0050003u
#define BAD_IN_A_ROW        16u                 /* whole transfers failed: stop asking */

/* Keep this stream going? Never queue again on a stream failing at once: its completions
 * would chain for ever at DISPATCH_LEVEL and take the console with them. */
static u32 keep_going(UAC *u, u32 *statuses, u32 bad)
{
    if (u->stopping)
        return 0;
    if (!statuses || bad == UAC_PACKETS) {
        if (++u->bad_in_a_row >= BAD_IN_A_ROW) {
            u->stopping = 1;
            u->st->gave_up = 1;
            return 0;
        }
    } else {
        u->bad_in_a_row = 0;
    }
    return 1;
}

/* One transfer's frames from the ring, and its packets' lengths. */
static void fill(UAC *u, u32 slot)
{
    const UAC_STREAM *p = &u->info.play;
    u32 packet, frames = 0, one = p->channels * p->bytes;

    for (packet = 0; packet < UAC_PACKETS; packet++) {
        u32 n = pacer_next(&u->pacer);

        u->play_lengths[slot][packet] = (u16)(n * one);
        frames += n;
    }
    audio_read(u->frames, frames);
    pcm_to_packet(u->frames, frames, u->play_buf[slot], p->bytes);
    u->play[slot].Length = frames * one;
}

static void play_done(ISOCH *t, u32 *statuses, u16 *lengths)
{
    UAC *u = (UAC *)t->Owner;
    u32 slot = (u32)(t - u->play), i, bad = 0;

    (void)lengths;
    u->st->transfers++;
    for (i = 0; statuses && i < UAC_PACKETS; i++) {
        if (statuses[i] == 0) {
            u->st->packets_ok++;
        } else if (statuses[i] == STATUS_LATE) {
            u->st->packets_late++;
        } else {
            u->st->packets_bad++;
            u->st->last_status = statuses[i];
            bad++;
        }
    }
    if (!keep_going(u, statuses, bad))
        return;
    fill(u, slot);
    usb_isoch(u->dev, t, u->play_lengths[slot]);
}

static void feedback_done(ISOCH *t, u32 *statuses, u16 *lengths)
{
    UAC *u = (UAC *)t->Owner;
    u32 slot = (u32)(t - u->feedback), i, room = u->info.play.feedback_max_packet, bad = 0;

    for (i = 0; statuses && lengths && i < UAC_PACKETS; i++) {
        if (statuses[i] != 0 && statuses[i] != STATUS_UNDERRUN && statuses[i] != STATUS_LATE)
            bad++;
        if ((statuses[i] == 0 || statuses[i] == STATUS_UNDERRUN) && lengths[i] &&
            pacer_feedback(&u->pacer, u->feedback_buf[slot] + i * room, lengths[i])) {
            u->st->feedback_taken++;
            u->st->feedback_rate = u->pacer.rate;
        }
    }
    if (!keep_going(u, statuses, bad))
        return;
    for (i = 0; i < UAC_PACKETS; i++)
        u->feedback_lengths[slot][i] = (u16)room;
    usb_isoch(u->dev, t, u->feedback_lengths[slot]);
}

u32 stream_start(UAC *u)
{
    const UAC_STREAM *p = &u->info.play;
    u32 slot, i, room = uac_packet_bytes(p) * UAC_PACKETS;

    u->bad_in_a_row = 0;
    pacer_begin(&u->pacer, p->sync == UAC_SYNC_ASYNC, UAC_MOST_FRAMES);
    for (slot = 0; slot < UAC_SLOTS; slot++) {
        if (!u->play_buf[slot])
            u->play_buf[slot] = (u8 *)net_dma_scratch(room);
        if (!u->play_buf[slot])
            return 1;
        u->play[slot].Context = u->ep_play;
        u->play[slot].Routine = play_done;
        u->play[slot].Endpoint = u->ep_play;
        u->play[slot].Buffer = u->play_buf[slot];
        u->play[slot].PacketCount = UAC_PACKETS;
        u->play[slot].Owner = u;
    }
    if (u->ep_feedback) {
        u32 fb = p->feedback_max_packet * UAC_PACKETS;

        for (slot = 0; slot < UAC_SLOTS; slot++) {
            if (!u->feedback_buf[slot])
                u->feedback_buf[slot] = (u8 *)net_dma_scratch(fb);
            if (!u->feedback_buf[slot])
                return 1;
            u->feedback[slot].Context = u->ep_feedback;
            u->feedback[slot].Routine = feedback_done;
            u->feedback[slot].Endpoint = u->ep_feedback;
            u->feedback[slot].Buffer = u->feedback_buf[slot];
            u->feedback[slot].Length = fb;
            u->feedback[slot].PacketCount = UAC_PACKETS;
            u->feedback[slot].Owner = u;
            for (i = 0; i < UAC_PACKETS; i++)
                u->feedback_lengths[slot][i] = (u16)p->feedback_max_packet;
            usb_isoch(u->dev, &u->feedback[slot], u->feedback_lengths[slot]);
        }
    }
    for (slot = 0; slot < UAC_SLOTS; slot++) {
        fill(u, slot);
        usb_isoch(u->dev, &u->play[slot], u->play_lengths[slot]);
    }
    return 0;
}
