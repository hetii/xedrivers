/* The console's own network stack (make stack=1): lwIP in place of xnet, for everything.
 * Once it is up, every frame the cards bring is its, and every frame xnet sends is dropped:
 * xnet is left running with nothing to do. Programs reach it through xam's NetDll_*
 * exports, which winsock/ points here; the address is its own, from DHCP. This header is
 * what src/net sees of it; with stack=0 every call here is nothing.
 *
 *   port/      lwIP on this machine: compiler and options, the kernel as its operating
 *              system (sysarch.c), the C library functions it calls (libc.c)
 *   link.c     the network interface: frames in from the cards (a ring, a thread), frames
 *              out to the uplink (a queue), DHCP on the uplink's MAC
 *   winsock/   xnet's socket interface on lwIP, for every program
 *   ownsock.c  the file servers' sockets straight on lwIP (`stack_servers = 1`), to set
 *              against the same servers through winsock/
 * lwIP itself is not in the repository: the Makefile fetches the release, pinned by
 * sha256, as it does the firmware.
 */
#ifndef XE_IPSTACK_H
#define XE_IPSTACK_H

#include "net.h"


typedef struct {
    u32 up;                     /* lwIP is running */
    u32 ip;                     /* the address it has (xnet's, learnt from its frames) */
    u32 rx;                     /* frames handed to lwIP */
    u32 rx_dropped;             /* the ring from the cards was full */
    u32 rx_no_pbuf;             /* lwIP had no buffer */
    u32 rx_refused;             /* lwIP's input returned an error */
    u32 rx_most;                /* frames waiting in the ring at most */
    u32 tx;                     /* frames lwIP sent */
    u32 tx_dropped;             /* the queue to the card was full */
    u32 tx_most;
    u32 tx_starved;             /* times lwIP was told to send again after the queue emptied */
    u32 asserts;                /* lwIP's assertions that failed, and the last one's text */
    const char *assert_text;
    u32 pbufs_used, pbufs_most; /* of lwIP's pool of receive buffers (PBUF_POOL_SIZE) */
    u32 pbufs_refused;          /* times the pool had none */
    u32 gateway, dns;           /* what DHCP gave */
    u32 dhcp_starts;            /* DHCP begun: once, and again for another uplink MAC */
    u32 xnet_dropped;           /* frames xnet sent, dropped */
} IPSTACK_STATS;

#if XE_STACK

extern volatile IPSTACK_STATS ipstack_stats;
extern volatile u32 ipstack_ready;

void ipstack_init(void);                /* thread */
/* 1: the file servers' sockets are the stack's (`stack_servers = 1` in the ini). */
extern volatile u32 ipstack_servers;
s32  own_listen(u32 port, u32 backlog);
s32  own_udp(u32 port);
s32  own_accept(s32 listener, u32 *peer);
s32  own_connect(u32 address, u32 port);
s32  own_recv(s32 s, void *buffer, u32 length);
s32  own_send(s32 s, const void *buffer, u32 length);
s32  own_recvfrom(s32 s, void *buffer, u32 length, u32 *address, u32 *port);
s32  own_sendto(s32 s, const void *buffer, u32 length, u32 address, u32 port);
s32  own_wait(s32 s, u32 ms);
void own_close(s32 s);
u32  own_local_port(s32 s);
u32  own_host_address(void);
u32  ipstack_rx_slow(const u8 *frame, u32 length);
void ipstack_tx_done(void);

/* A frame from a card, before xnet: 1 once the stack is up -- every frame is its. */
static inline u32 ipstack_rx(const u8 *frame, u32 length)
{
    return ipstack_ready ? ipstack_rx_slow(frame, length) : 0;
}

/* A frame xnet sends: 1 once the stack is up, and it is dropped. */
static inline u32 ipstack_takes_tx(void)
{
    if (!ipstack_ready)
        return 0;
    ipstack_stats.xnet_dropped++;
    return 1;
}

/* net.c: the uplink's own transmit; 0 when the card has no room. */
u32 net_stack_tx(const u8 *frame, u32 length);

#else

#define ipstack_servers 0u
static inline void ipstack_init(void) { }
static inline u32  ipstack_rx(const u8 *frame, u32 length) { (void)frame; (void)length; return 0; }
static inline u32  ipstack_takes_tx(void) { return 0; }
static inline void ipstack_tx_done(void) { }

#endif

#endif /* XE_IPSTACK_H */
