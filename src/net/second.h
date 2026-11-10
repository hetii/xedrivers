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
    u32 out_refused;        /* the card had no room */
    u32 flows;              /* learned */
    u32 not_ours;           /* on the second card, for neither address */
    u32 no_xnet_ip;         /* came before xnet had said its address */
} SECOND_STATS;

/* --- pure (tests/net/test_second.c) ----------------------------------------- */

#define SECOND_FLOWS 64u    /* a power of two */

typedef struct {
    u32 peer_ip;
    u32 ports;              /* peer's << 16 | ours */
    u32 proto;              /* 0: free */
    u8  peer_mac[ETH_ALEN]; /* where its frames came from: the way back */
} SECOND_FLOW;

typedef struct {
    SECOND_FLOW flow[SECOND_FLOWS];
    u32 learned;
} SECOND_TABLE;

/* The checksum at `sum` (big endian) after 32 bits of what it covers changed. */
void second_fix(u8 *sum, u32 was, u32 is);

/* An IPv4 frame's flow, as seen from the console: 1 and the key, or 0 for what has none
 * (not IPv4, a later fragment, a protocol without ports other than ICMP). `in`: the frame
 * came in (the peer is its source). */
u32  second_key(const u8 *frame, u32 length, u32 in, u32 *peer_ip, u32 *ports, u32 *proto);

void second_learn(SECOND_TABLE *t, u32 peer_ip, u32 ports, u32 proto, const u8 *peer_mac);
const SECOND_FLOW *second_find(const SECOND_TABLE *t, u32 peer_ip, u32 ports, u32 proto);

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

/* A frame from a card that is not the uplink: 1 if it was the second way's. DISPATCH. */
static inline u32 second_rx(NET_DEV *nd, const u8 *frame, u32 length)
{
    return nd == second_dev ? second_rx_slow(nd, frame, length) : 0;
}

/* A frame from xnet: 1 if it went (or was refused, `*sent` 0) by the second card. */
static inline u32 second_tx(const u8 *frame, u32 length, u32 *sent)
{
    return second_dev ? second_tx_slow(frame, length, sent) : 0;
}

/* Its completions wake xnet's queue too. */
static inline u32 second_is(const NET_DEV *nd)
{
    return nd == second_dev;
}
#else
static inline void second_init(void) { }
static inline void second_choose(void) { }
static inline u32  second_rx(NET_DEV *nd, const u8 *frame, u32 length)
{ (void)nd; (void)frame; (void)length; return 0; }
static inline u32  second_tx(const u8 *frame, u32 length, u32 *sent)
{ (void)frame; (void)length; (void)sent; return 0; }
static inline u32  second_is(const NET_DEV *nd) { (void)nd; return 0; }
#endif

#endif /* XE_SECOND_H */
