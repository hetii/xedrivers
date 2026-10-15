/* A tiny IPv4 responder that runs on the card alone, without xnet: ARP, ICMP echo,
 * UDP echo (7) and discard (9), and a UDP generator for throughput runs. It is how
 * the driver is measured with the console's own network untouched -- 192.168.1.210
 * keeps working over the onboard NIC the whole time. */

#ifndef XE_STACK_H
#define XE_STACK_H

#include "kernel.h"

extern volatile u32 stack_ip;           /* ours, host order; default 10.36.0.2 */

void stack_input(const u8 *frame, u32 length);

/* Thread-only. Start sending `count` UDP datagrams of `payload` bytes to the last
 * peer that spoke to us, on `port`, and return; count 0 stops a run in progress.
 * Progress: stack_stats.blast_sent / blast_busy. */
u32  stack_blast(u32 count, u32 payload, u32 port);

typedef struct {
    u32 arp_replies;
    u32 icmp_replies;
    u32 udp_echo;
    u32 discard_frames;
    u32 discard_bytes;
    u32 discard_bytes_hi;
    u32 peer_ip;
    u32 peer_mac_hi;
    u32 peer_mac_lo;
    u32 blast_sent;
    u32 blast_busy;
    u32 not_ours;
} STACK_STATS;

extern volatile STACK_STATS stack_stats;

/* Pure helpers, exported for tests/. */
u16 ip_checksum(const u8 *data, u32 length, u32 initial);

#endif /* XE_STACK_H */
