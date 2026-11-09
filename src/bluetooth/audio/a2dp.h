/* A2DP, the console as an audio source (A2DP 1.4, AVDTP 1.3): one headset at a time. */

#ifndef XE_A2DP_H
#define XE_A2DP_H

#include "host.h"
#include "sbc.h"

#define A2DP_IDLE        0u
#define A2DP_SIGNALLING  1u     /* the signalling channel is being opened */
#define A2DP_DISCOVERING 2u
#define A2DP_CAPABILITIES 3u
#define A2DP_CONFIGURING 4u
#define A2DP_OPENING     5u     /* OPEN sent, or the media channel is being opened */
#define A2DP_OPEN        6u     /* configured, the media channel there, not started */
#define A2DP_STARTING    7u
#define A2DP_STREAMING   8u
#define A2DP_RECONF_CAPS 9u     /* the sink configured it: asking what else it takes */
#define A2DP_RECONFIGURING 10u

/* The SBC codec information element, four bytes (A2DP 4.3.2). */
typedef struct {
    u8 b[4];
} SBC_CAPS;

typedef struct {
    u32 state;
    u32 role_initiator;         /* we opened the signalling channel */
    u32 remote_seid;
    u32 chose_rate, chose_mode, chose_blocks, chose_bitpool, chose_allocation;
    u32 caps_seen[2];           /* the sink's SBC element, and its bitpool range */
    u32 frame_length;
    u32 frames_per_packet;
    u32 packets;
    u32 frames;
    u32 send_failures;          /* no ACL credit when due: the radio lags */
    u32 late;                   /* samples skipped: the worker fell behind */
    u32 rejects;
    u32 last_reject;            /* signal << 8 | error */
    u32 starts;
    u32 media_mtu;
    u32 bitpool_now;            /* following the radio (a2dp.c, bitpool_tick) */
    u32 bitpool_lowest;
    u32 bitpool_downs;
    u32 reconfigured;           /* the sink's choice replaced by a better one */
    u32 led_for_sink;           /* the sink opened signalling and waited: we led */
} A2DP_STATS;

extern volatile A2DP_STATS a2dp_stats;

/* Pure (tests/bluetooth): the best configuration a sink's SBC capabilities allow, written
 * as the element SET_CONFIGURATION sends; 0 if there is none. Highest sampling rate up to
 * 48 kHz, then the mode and bitpool that give the most bits a second. */
u32  a2dp_choose(const SBC_CAPS *sink, u32 min_bitpool, u32 max_bitpool, SBC_CAPS *out,
                 u32 *bitpool);

void a2dp_init(void);
void a2dp_connect(volatile BT_CONN *c);     /* a paired headset's link is secure */
void a2dp_gone(volatile BT_CONN *c);
void a2dp_tick(void);                       /* the worker's, every tick */

#endif /* XE_A2DP_H */
