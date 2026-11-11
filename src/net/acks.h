/* TCP acknowledgements thinned on the way out.
 *
 * xnet acknowledges every second segment it receives. On this console each of those is a
 * bulk OUT transaction on the same host controller that is busy bringing the data in, and
 * the controller does one transaction a queue head a pass of its ring: receiving 4 TCP
 * streams it took 215.6 Mbit/s (26.9 MB/s) while sending 9.4k acknowledgements a second,
 * and 243.8 Mbit/s (30.5 MB/s) of UDP, which is not acknowledged (2026-11-12, one RTL8156B).
 * An acknowledgement is cumulative: a later one says all an earlier one did. So of a
 * connection's pure acknowledgements only every `net_ack_thin`-th is sent, the newest of
 * those held going out when none has gone for ACK_HOLD_US, with the next frame received or
 * the worker's tick. Never held: one that repeats the number before it (a duplicate tells
 * the sender of a loss, three of them make it resend), one with a SACK option, and anything
 * that is not an acknowledgement alone. A frame of the connection that carries data, or
 * FIN, takes the held one's place: it acknowledges too.
 */

#ifndef XE_ACKS_H
#define XE_ACKS_H

#include "net.h"

#define ACK_FLOWS    16u        /* a power of two; one connection a place */
#define ACK_FRAME    96u        /* Ethernet + IP + TCP with options, no payload */
#define ACK_HOLD_US  500u

typedef struct {
    u32 peer_ip;
    u32 ports;                  /* ours << 16 | the peer's; 0: free */
    u32 last;                   /* the number the last pure acknowledgement carried */
    u32 skipped;                /* held or dropped since one was sent */
    u32 sent_at;                /* clock at the last one sent */
    u32 held;                   /* bytes of `frame` waiting, 0 for none */
    u8  frame[ACK_FRAME];
} ACK_FLOW;

typedef struct {
    ACK_FLOW flow[ACK_FLOWS];
    u32 held_now;               /* how many places hold one */
} ACK_TABLE;

/* What a frame from xnet is: 0 not TCP over IPv4 (or a fragment), 1 TCP that is no pure
 * acknowledgement (or one with SACK), 2 a pure acknowledgement. The connection and the
 * acknowledgement number are given for 1 and 2. */
u32 ack_kind(const u8 *frame, u32 length, u32 *peer_ip, u32 *ports, u32 *number);

/* A frame on its way out at clock `now` (`hold` in the clock's units). Returns 1 when it
 * was a pure acknowledgement kept back (the caller says it was sent), 0 when the caller is
 * to send it. `*flush` is set to a place whose held acknowledgement has to go out first
 * (another connection wanted it), else 0. */
u32 ack_out(ACK_TABLE *t, const u8 *frame, u32 length, u32 thin, u32 now, u32 hold,
            ACK_FLOW **flush);

/* The next place whose held acknowledgement is due (`force`: any that holds one), 0 for
 * none. The caller sends flow->frame, flow->held bytes, then says ack_sent. */
ACK_FLOW *ack_due(ACK_TABLE *t, u32 now, u32 hold, u32 force);
void ack_sent(ACK_TABLE *t, ACK_FLOW *flow, u32 now);

/* --- the plugin's ----------------------------------------------------------- */

typedef struct {
    u32 seen;                   /* pure acknowledgements from xnet */
    u32 sent;                   /* of them, sent as they came */
    u32 dropped;                /* never sent: a later one took their place */
    u32 late;                   /* held ones sent by the hold running out */
    u32 duplicates;             /* repeated numbers, always sent */
} ACK_STATS;

extern volatile ACK_STATS net_ack_stats;
extern volatile u32 net_ack_thin;       /* send one in this many; 0 or 1: all of them */
extern volatile u32 net_ack_holding;    /* places that hold one now */

void acks_init(void);                   /* thread: the ini (`ack_thin =`) */
/* net_tx: 1 if the frame was kept back. `send` is the uplink's own transmit. */
u32  acks_tx(const u8 *frame, u32 length, u32 (*send)(const u8 *, u32));
/* Held ones that are due; `force` for all. Any IRQL up to DISPATCH. */
void acks_flush(u32 (*send)(const u8 *, u32), u32 force);

#endif /* XE_ACKS_H */
