/* Transmit with backpressure; see txq.h. */

#include "txq.h"

static u32 count(const NET_TXQ *t)
{
    return (t->tail + NET_TXQ_SIZE - t->head) % NET_TXQ_SIZE;
}

static void dropped(NET_TXQ *t)
{
    if (t->dropped)
        (*t->dropped)++;
}

void net_txq_drain(NET_TXQ *t)
{
    NET_TXQ_ENTRY e;
    u32 irql, sent;

    for (;;) {
        if (__sync_lock_test_and_set(&t->draining, 1))
            return;
        for (;;) {
            irql = KfAcquireSpinLock(&t->lock);
            if (t->head == t->tail || !t->space(t->owner)) {
                KfReleaseSpinLock(&t->lock, irql);
                break;
            }
            e = t->q[t->head];
            t->head = (t->head + 1) % NET_TXQ_SIZE;
            KfReleaseSpinLock(&t->lock, irql);

            sent = t->send(t->owner, (const u8 *)e.frame, e.length);
            if (!sent && t->keep(t->owner)) {
                /* The space hint raced a fill: put it back where it was. Only
                 * the drainer takes from the head, so this cannot reorder. */
                irql = KfAcquireSpinLock(&t->lock);
                t->head = (t->head + NET_TXQ_SIZE - 1) % NET_TXQ_SIZE;
                t->q[t->head] = e;
                KfReleaseSpinLock(&t->lock, irql);
                break;
            }
            if (!sent)
                dropped(t);
            t->done(t->owner, e.user, e.cookie);
        }
        __sync_lock_release(&t->draining);
        if (t->head == t->tail || !t->space(t->owner))
            break;
    }
}

void net_txq_put(NET_TXQ *t, void *user, void *frame, u32 length, void *cookie)
{
    u32 irql, depth;

    irql = KfAcquireSpinLock(&t->lock);
    if ((t->tail + 1) % NET_TXQ_SIZE == t->head) {
        KfReleaseSpinLock(&t->lock, irql);
        dropped(t);
        t->done(t->owner, user, cookie);
        return;
    }
    t->q[t->tail].user = user;
    t->q[t->tail].frame = frame;
    t->q[t->tail].length = length;
    t->q[t->tail].cookie = cookie;
    t->tail = (t->tail + 1) % NET_TXQ_SIZE;
    depth = count(t);
    if (t->high && depth > *t->high)
        *t->high = depth;
    KfReleaseSpinLock(&t->lock, irql);
    net_txq_drain(t);
}

void net_txq_flush(NET_TXQ *t)
{
    NET_TXQ_ENTRY e;
    u32 irql;

    for (;;) {
        irql = KfAcquireSpinLock(&t->lock);
        if (t->head == t->tail) {
            KfReleaseSpinLock(&t->lock, irql);
            return;
        }
        e = t->q[t->head];
        t->head = (t->head + 1) % NET_TXQ_SIZE;
        KfReleaseSpinLock(&t->lock, irql);
        dropped(t);
        t->done(t->owner, e.user, e.cookie);
    }
}
