/* A second way into the console (make multipath=1, `second_ip =` in xedrivers.ini): a card
 * that is not the uplink answers for an address of its own, and what comes for that address
 * is handed to xnet as if it had come for the console's -- xnet has one interface and one
 * address. The answers xnet gives to such a peer leave by the same card, from that address.
 * A client that opens connections to both addresses fills both cards: on two host
 * controllers each has its own ceiling (docs/xedrivers.md, Two cards at once).
 *
 * Address translation, per flow (protocol, peer address and port, console port), learned from
 * what comes in on the second card: nothing the console opens itself goes that way. Without
 * the build flag there is none of it; with it and no `second_ip`, one test of a flag a frame.
 */

#ifndef XE_SECOND_H
#define XE_SECOND_H

#include "net.h"

#ifndef XE_MULTIPATH
#define XE_MULTIPATH 0
#endif

typedef struct {
    u32 ip;                 /* ours on the second card, host order; 0: off */
    u32 xnet_ip;            /* the console's, learned from what xnet sends */
    u32 slot;               /* the card's slot + 1, 0 without one */
    u32 arp_replies;
    u32 in;                 /* frames handed to xnet */
    u32 out;                /* frames sent for it */
    u32 out_refused;        /* (no longer counted: frames wait in the second way's own queue) */
    u32 flows;              /* learned */
    u32 not_ours;           /* on the second card, for neither address */
    u32 no_xnet_ip;         /* came before xnet had said its address */
    u32 out_dropped;        /* the second way's queue was full */
    u32 gave_up;            /* ... for 2 s without a frame taken: the second way given up */
    u32 moved;              /* a flow opened anew by the uplink: no longer the second way's */
    u32 announced;          /* the address said to be a card's, when the card changed */
    u32 queued;             /* frames waiting for the second card now */
    u32 queued_most;
} SECOND_STATS;

/* --- pure (tests/net/test_second.c) ----------------------------------------- */

#define SECOND_FLOWS 128u   /* a power of two */
#define SECOND_IDLE  45u    /* table ticks (1.35 s each) a flow without a connection is kept */

typedef struct {
    u32 peer_ip;
    u32 ports;              /* peer's << 16 | ours */
    u32 proto;              /* 0: free */
    u32 seen;               /* table tick of its last frame, either way */
    u8  peer_mac[ETH_ALEN]; /* where its frames came from: the way back */
} SECOND_FLOW;

typedef struct {
    SECOND_FLOW flow[SECOND_FLOWS];
    u32 learned;
} SECOND_TABLE;

/* A card that takes nothing: how long since, and what to do with the frame it refused. */
typedef struct {
    u32 refusing;           /* since `since`, without one frame taken in between */
    u32 since;
} SECOND_STUCK;

#define SECOND_WAIT 0u      /* back to xnet's queue: the card is only busy */
#define SECOND_DROP 1u      /* gone, as lost on the wire: the queue is the uplink's too */
#define SECOND_QUIT 2u      /* and the second way is given up */

/* The checksum at `sum` (big endian) after 32 bits of what it covers changed. */
void second_fix(u8 *sum, u32 was, u32 is);

/* An IPv4 frame's flow, as seen from the console: 1 and the key, or 0 for what has none
 * (not IPv4, a later fragment, a protocol without ports other than ICMP). `in`: the frame
 * came in (the peer is its source). */
u32  second_key(const u8 *frame, u32 length, u32 in, u32 *peer_ip, u32 *ports, u32 *proto);

/* `now` is a tick of any length that SECOND_IDLE is counted in. A table that is full where
 * a new flow belongs gives it the place of the one unused longer. */
void second_learn(SECOND_TABLE *t, u32 peer_ip, u32 ports, u32 proto, const u8 *peer_mac,
                  u32 now);
/* The flow, used now; 0 for none, and for one that is no TCP connection and was idle for
 * SECOND_IDLE (a TCP connection may be idle for as long as it likes). */
const SECOND_FLOW *second_find(SECOND_TABLE *t, u32 peer_ip, u32 ports, u32 proto, u32 now);
u32  second_forget(SECOND_TABLE *t, u32 peer_ip, u32 ports, u32 proto);

/* The card had no room at `now`: SECOND_WAIT until that has lasted `hold`, SECOND_DROP
 * after, SECOND_QUIT once it has lasted `quit`. A frame it takes ends it (second_took). */
u32  second_refused(SECOND_STUCK *s, u32 now, u32 hold, u32 quit);
void second_took(SECOND_STUCK *s);

/* The frame's source or destination address (`dest`) and Ethernet address rewritten, the IP
 * and TCP/UDP checksums kept right. */
void second_rewrite(u8 *frame, u32 dest, u32 ip, const u8 *mac);

/* --- the plugin's ----------------------------------------------------------- */

#if XE_MULTIPATH
extern volatile SECOND_STATS second_stats;

/* The second way's card, 0 while there is none (no `second_ip`, or no card left for it):
 * what every frame is tested against, here, so that a console with one card pays a load
 * and a compare a frame and no call. */
extern NET_DEV *volatile second_dev;

void second_init(void);                         /* thread: the ini */
/* net.c, choose(): a link, a claim or the uplink changed. Any IRQL up to DISPATCH. */
void second_choose(void);
u32  second_rx_slow(NET_DEV *nd, const u8 *frame, u32 length);
u32  second_tx_slow(const u8 *frame, u32 length, u32 *sent);
void second_uplink_slow(const u8 *frame, u32 length);
void second_drain(void);

/* A frame from a card that is not the uplink: 1 if it was the second way's. DISPATCH. */
static inline u32 second_rx(NET_DEV *nd, const u8 *frame, u32 length)
{
    return second_dev ? second_rx_slow(nd, frame, length) : 0;
}

/* A frame from xnet: 1 if it went (or was refused, `*sent` 0) by the second card. */
static inline u32 second_tx(const u8 *frame, u32 length, u32 *sent)
{
    return second_dev ? second_tx_slow(frame, length, sent) : 0;
}

/* A frame from the uplink, before xnet has it: a connection opened anew by that way is
 * not the second way's any more, whatever the table still says of its addresses. */
static inline void second_uplink_rx(const u8 *frame, u32 length)
{
    if (second_dev)
        second_uplink_slow(frame, length);
}

static inline u32 second_is(const NET_DEV *nd)
{
    return nd == second_dev;
}

/* A transmit of `nd` completed: the second way's queue goes on if it is its card. */
static inline void second_tx_done(const NET_DEV *nd)
{
    if (nd == second_dev)
        second_drain();
}
#else
static inline void second_init(void) { }
static inline void second_choose(void) { }
static inline u32  second_rx(NET_DEV *nd, const u8 *frame, u32 length)
{ (void)nd; (void)frame; (void)length; return 0; }
static inline u32  second_tx(const u8 *frame, u32 length, u32 *sent)
{ (void)frame; (void)length; (void)sent; return 0; }
static inline u32  second_is(const NET_DEV *nd) { (void)nd; return 0; }
static inline void second_uplink_rx(const u8 *frame, u32 length) { (void)frame; (void)length; }
static inline void second_tx_done(const NET_DEV *nd) { (void)nd; }
#endif

#endif /* XE_SECOND_H */
