/* Transmit with backpressure, for a consumer that is handed frames by the console's
 * network stack -- xnet's NicXmit hook (xnet.c), a wireless card's Xmit service
 * (src/wireless/omni.c).
 *
 * A NIC tells its user a frame is sent when it is, and xnet does not reuse the
 * buffer before. Completing every frame on the spot and dropping it when the USB
 * ring was full lost ~0.3% of frames under load (1918 of 576638 in four parallel
 * FTP downloads) and TCP spent the rest of the run in retransmission timeouts:
 * one stream 157 Mbit/s, four streams 14 between them.
 *
 * So frames queue here, in order, with the buffer and cookie xnet gave, and leave
 * for the ring as slots free up; each is completed only once it has been copied
 * into a transfer buffer. One drainer at a time, chosen by a test-and-set flag
 * rather than a spin lock: completing a frame re-enters xnet, which may hand a
 * frame straight back, and a completion of our own transfer runs the drain from
 * the USB processor. Whoever finds the flag taken leaves the work to its holder,
 * who looks again after letting go.
 */

#ifndef XE_TXQ_H
#define XE_TXQ_H

#include "kernel.h"

#define NET_TXQ_SIZE 512

typedef struct {
    void *user;
    void *frame;
    u32   length;
    void *cookie;
} NET_TXQ_ENTRY;

typedef struct {
    /* Put one frame in the ring: 1 if it went. */
    u32  (*send)(void *owner, const u8 *frame, u32 length);
    /* Whether a slot is free: a hint. */
    u32  (*space)(void *owner);
    /* Whether a frame that did not go is worth keeping for later (a link). */
    u32  (*keep)(void *owner);
    /* The frame is done with, sent or not: hand it back to its user. */
    void (*done)(void *owner, void *user, void *cookie);
    void *owner;
    volatile u32 *dropped;      /* completed unsent: queue full or link gone */
    volatile u32 *high;         /* the deepest the queue has been */

    NET_TXQ_ENTRY q[NET_TXQ_SIZE];
    u32 head, tail;
    KSPIN_LOCK lock;
    volatile u32 draining;
} NET_TXQ;

/* Queue one frame and drain what fits; a full queue completes it unsent. Any IRQL
 * up to DISPATCH. */
void net_txq_put(NET_TXQ *t, void *user, void *frame, u32 length, void *cookie);

/* Move what fits into the ring: call when a slot frees up. */
void net_txq_drain(NET_TXQ *t);

/* Complete everything still waiting, unsent: the link went away. */
void net_txq_flush(NET_TXQ *t);

#endif /* XE_TXQ_H */
