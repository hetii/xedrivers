/* Putting a block ack's frames back in order.
 *
 * A firmware that hands an A-MPDU's frames up as they decode delivers one the
 * access point had to send again after those behind it: 3.7% of frames into the
 * console under load with the AIC8800, each one a duplicate ACK to the sender, a
 * fast retransmit and a halved window. Linux puts them back in order in mac80211
 * (agg-rx.c), aicsemi's fullmac driver in its own reord_process_unit, and this is
 * that: a window of 64 sequence numbers per TID, a frame at the head of the window
 * handed up with every one held behind it, one ahead of a hole copied aside, one
 * behind the window handed up as it came (reord_need_check's "no need to reord":
 * a duplicate costs the stack nothing, a frame dropped here costs a
 * retransmission). Single-threaded by contract; see reorder.h.
 */

#include "reorder.h"

#define SEQ_MASK 0xFFFu

void wl_reorder_reset(WL_REORDER *r)
{
    u32 t, i;

    for (t = 0; t < WL_REORDER_TIDS; t++) {
        r->tid[t].active = 0;
        r->tid[t].held = 0;
        r->tid[t].stalled = 0;
        r->tid[t].late_run = 0;
        for (i = 0; i < WL_REORDER_WINDOW; i++)
            r->tid[t].slot[i] = WL_REORDER_NONE;
    }
    for (i = 0; i < WL_REORDER_SLOTS; i++)
        r->free[i] = (u8)i;
    r->free_count = WL_REORDER_SLOTS;
}

void wl_reorder_init(WL_REORDER *r, u8 *pool, WL_DELIVER deliver, void *context,
                     volatile WL_REORDER_STATS *st)
{
    r->pool = pool;
    r->deliver = deliver;
    r->context = context;
    r->st = st;
    r->drop_late = 0;
    wl_reorder_reset(r);
}

static void release(WL_REORDER *r, WL_REORDER_TID *q, u32 index)
{
    u8 slot = q->slot[index];

    q->slot[index] = WL_REORDER_NONE;
    q->held--;
    r->deliver(r->context, r->pool + slot * WL_REORDER_FRAME, q->length[index]);
    r->free[r->free_count++] = slot;
}

/* Hand up what is consecutive from the head on. */
static void advance(WL_REORDER *r, WL_REORDER_TID *q)
{
    u32 index;

    for (;;) {
        index = q->head & (WL_REORDER_WINDOW - 1);
        if (q->slot[index] == WL_REORDER_NONE)
            break;
        release(r, q, index);
        q->head = (q->head + 1) & SEQ_MASK;
    }
    q->stalled = 0;
}

/* Give up on the holes: everything held goes up in order, the head after it. */
static void flush(WL_REORDER *r, WL_REORDER_TID *q)
{
    u32 n;

    for (n = 0; q->held && n < WL_REORDER_WINDOW; n++) {
        if (q->slot[q->head & (WL_REORDER_WINDOW - 1)] == WL_REORDER_NONE)
            q->head = (q->head + 1) & SEQ_MASK;
        else
            advance(r, q);
    }
    q->stalled = 0;
}

void wl_reorder_in(WL_REORDER *r, u32 tid, u32 seq, u8 *frame, u32 length)
{
    WL_REORDER_TID *q = &r->tid[tid & (WL_REORDER_TIDS - 1)];
    u32 distance, index, i;
    u8 slot, *to;

    if (!r->pool || length > WL_REORDER_FRAME) {
        r->deliver(r->context, frame, length);
        return;
    }
    if (!q->active) {
        q->active = 1;
        q->head = (u16)seq;
    }
    distance = (seq - q->head) & SEQ_MASK;
    if (distance >= 0x800) {
        /* Behind the window: already handed up, or given up on. Many in a row
         * mean the sender's numbering started again: follow it. */
        r->st->late++;
        if (++q->late_run < WL_REORDER_RESYNC) {
            if (!r->drop_late)
                r->deliver(r->context, frame, length);
            return;
        }
        r->st->resyncs++;
        flush(r, q);
        q->head = (u16)seq;
        distance = 0;
    }
    q->late_run = 0;
    if (distance >= WL_REORDER_WINDOW) {
        /* Past the end of the window: move it just far enough that this frame is
         * its last, letting go of what falls out of it (as Linux does). */
        u32 new_head = (seq - (WL_REORDER_WINDOW - 1)) & SEQ_MASK;

        r->st->jumps++;
        while (q->head != new_head) {
            index = q->head & (WL_REORDER_WINDOW - 1);
            if (q->slot[index] != WL_REORDER_NONE)
                release(r, q, index);
            q->head = (q->head + 1) & SEQ_MASK;
        }
        advance(r, q);
        distance = (seq - q->head) & SEQ_MASK;
    }
    if (!distance) {
        r->deliver(r->context, frame, length);
        q->head = (q->head + 1) & SEQ_MASK;
        advance(r, q);
        return;
    }
    index = seq & (WL_REORDER_WINDOW - 1);
    if (q->slot[index] != WL_REORDER_NONE) {
        r->st->late++;                      /* the same one twice */
        return;
    }
    if (!r->free_count) {
        r->st->full++;
        flush(r, q);
        r->deliver(r->context, frame, length);
        q->head = (u16)((seq + 1) & SEQ_MASK);
        return;
    }
    slot = r->free[--r->free_count];
    to = r->pool + slot * WL_REORDER_FRAME;
    for (i = 0; i < length; i++)
        to[i] = frame[i];
    q->slot[index] = slot;
    q->length[index] = (u16)length;
    q->held++;
    r->st->held++;
}

void wl_reorder_end(WL_REORDER *r, u32 tid)
{
    WL_REORDER_TID *q = &r->tid[tid & (WL_REORDER_TIDS - 1)];

    if (!r->pool || !q->active)
        return;
    flush(r, q);
    q->active = 0;
    r->st->ended++;
}

u32 wl_reorder_tick(WL_REORDER *r)
{
    u32 t, due = 0;

    if (!r->pool)
        return 0;
    for (t = 0; t < WL_REORDER_TIDS; t++)
        if (r->tid[t].held && ++r->tid[t].stalled >= WL_REORDER_TICKS)
            due = 1;
    return due;
}

void wl_reorder_expire(WL_REORDER *r)
{
    u32 t;

    for (t = 0; t < WL_REORDER_TIDS; t++)
        if (r->tid[t].held && r->tid[t].stalled >= WL_REORDER_TICKS) {
            r->st->timeouts++;
            flush(r, &r->tid[t]);
        }
}
