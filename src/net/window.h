/* A larger TCP receive window than xnet has.
 *
 * xnet's receive window is its socket buffer: 128 KB for system sockets as we set it
 * (xnet.c), 16 KB where a program chose (XBDM, ftpdll), and its pool cannot hold much
 * more. Over Wi-Fi that window is what stops the sender: 29% of an RTL8822BU's frames
 * arrive out of order and wait in the reorder buffer for the access point to send the
 * missing one again, up to 64 frames (96 KB) at a time, and until they reach xnet nothing
 * is acknowledged. One TCP stream into the console did 140.5 Mbit/s with 63 KB, 185.5
 * with 128, 203.0 with 255, where the card takes 235 of UDP (2026-11-12).
 *
 * So the window the peer sees is made larger here, and what it sends beyond xnet's own
 * is kept here until xnet has room:
 *   - out: in every segment of a connection whose opening was seen, the window field says
 *     xnet's right edge plus `extra` (less what is held), and the window scale xnet offered
 *     in its SYN is replaced by WIN_SHIFT so the field can say it. The frame is xnet's and
 *     may be sent again from the same bytes: what was changed is put back after the card
 *     has copied it (WIN_PATCH).
 *   - in: a segment that ends beyond xnet's right edge (as its last segment out told it) is
 *     copied to a pool and handed up, in the order it came, when the edge has moved past
 *     it. One that fits goes up at once, whatever is held: it is older than all of them.
 * What is told beyond xnet's edge is never more than the pool can keep: `extra`, or the
 * pool divided among the connections receiving now. Told more (four connections by two
 * cards, 256 KB each over a pool of 384 KB), the pool ran out, the segments it could not
 * keep were dropped by xnet, and with no SACK each cost a round trip or a timeout: 349.9
 * Mbit/s into the console by two addresses fell to 246.4, four FTP sessions 305.8 to 111.3.
 * And it is told only while it helps. A frame that has to be kept means xnet (or the
 * program reading) is behind, not the sender: then a larger window only brings more to
 * copy and hand up later, on the processor that serves USB. By two cards that cost a
 * third and more (344.0-361.1 Mbit/s into the console by two addresses; 125.6 to 276.9 with
 * 256 KB told throughout, each run with a dozen 200 ms waits for an edge that did not
 * move; a grant cut by a quarter a tick still 217.1). So a connection's grant goes to
 * nothing in a tick in which a frame of it was kept and grows by WIN_GROW in one without:
 * 347.8-361.8 by two addresses, and the RTL8822BU alone keeps its gain. Not so for a
 * connection whose own window stays under WIN_SMALL (ftpdll's data socket states 64 KB,
 * XBDM's 16): its frames are kept nearly always, and that is where its gain is -- 159.3-
 * 165.1 Mbit/s over the RTL8822BU with the grant left, 123.0 with it cut, as without all
 * this. Handing a kept frame up early instead (a probe after 20 ms, not 200) made xnet
 * drop it: 47.0-49.0.
 * Acknowledgement numbers stay xnet's. A segment lost here (no room in the pool, a
 * connection closed) was never acknowledged, and the peer sends it again.
 *
 * Measured without / with, 256 KB (2026-11-13). RTL8822BU, 5 GHz, into the console: one TCP
 * stream 184.9-186.7 -> 210.4-211.6 Mbit/s, four 202.3-207.2 -> 212.5-216.5; xeftp 256 MB
 * 158.1-181.9 -> 188.0-210.8; ftpdll 112.4-122.3 -> 152.8-176.4. RTL8156B: ftpdll 173.5 ->
 * 202.5-204.2 (its sockets have 16 KB), TCP and xeftp as before (232, 225-233). Out of the
 * console nothing moves on either. 384 KB gave what 256 does. Few frames are held with a
 * reader as fast as the bench's (hundreds a run), a third of them with a file server's; 187
 * at most at once, so the pool is 256 frames: the console's system memory is short (a pool
 * of 1.5 MB left none, and threads then fail to start with 0xC000009A).
 *
 * `rx_window_kb = 0` in xedrivers.ini turns it off, a number sets `extra` in KB.
 */

#ifndef XE_WINDOW_H
#define XE_WINDOW_H

#include "net.h"

#define WIN_FLOWS       64u         /* a power of two; one connection a place */
#define WIN_FRAMES      256u        /* the pool, all connections: 384 KB */
#define WIN_FRAME       1536u
#define WIN_SHIFT       3u          /* told where xnet scales at all: up to 512 KB */
#define WIN_EXTRA_KB    256u
#define WIN_IDLE        500u        /* ticks (10 s) a place is kept for a quiet connection */
#define WIN_PROBE       10u         /* ticks (200 ms) without the edge moving: the first goes up */
#define WIN_BUSY        50u         /* ticks (1 s) since its last data: a connection that is receiving */
#define WIN_GROW        1024u       /* bytes a tick the grant grows back: all of it in 5 s */
#define WIN_SMALL       98304u      /* a connection whose own window stays under this keeps its grant */
#define WIN_NONE        0xFFFFu

typedef struct {
    u32 peer_ip;
    u32 ports;                  /* ours << 16 | the peer's; 0: free */
    u32 edge;                   /* xnet's right edge: its acknowledgement + its window */
    u32 told;                   /* the right edge the peer was told; never moves back */
    u32 held_bytes;
    u32 seen;                   /* tick of its last segment */
    u32 moved;                  /* tick the edge last moved, or a frame was first held */
    u32 data;                   /* tick of the last segment with data from the peer */
    u32 grant;                  /* what this connection is told beyond the edge now, at most `extra` */
    u32 kept;                   /* frames held since the last tick */
    u32 widest;                 /* the largest window xnet itself has stated */
    u16 first, last;            /* held frames, oldest first; WIN_NONE: none */
    u8  shift;                  /* xnet's own window scale */
    u8  told_shift;             /* the one the peer was given */
    u8  edge_known;
    u8  fins;                   /* 1 xnet sent FIN, 2 the peer did: both, and the place is free */
} WIN_FLOW;

typedef struct {
    WIN_FLOW flow[WIN_FLOWS];
    u8  *pool;                  /* WIN_FRAMES * WIN_FRAME bytes; 0: nothing is held */
    u32 end[WIN_FRAMES];        /* the sequence number after a held segment */
    u16 length[WIN_FRAMES];
    u16 payload[WIN_FRAMES];
    u16 next[WIN_FRAMES];
    u16 free;                   /* a list through next[] */
    u32 held_now, held_most;
    u32 opened, not_taken, dropped;
    u32 busy;                   /* connections receiving now: they share the pool */
} WIN_TABLE;

/* What win_out changed in a frame, to be put back by win_restore. */
typedef struct {
    u16 at[3];                  /* offsets of the bytes changed, 2 each */
    u8  old[3][2];
    u32 n;
} WIN_PATCH;

void win_reset(WIN_TABLE *t, u8 *pool);

/* A frame from xnet on its way out at tick `now`. Changes it in place (`p` says what) and
 * returns the connection when it has held frames that now fit, else 0. */
WIN_FLOW *win_out(WIN_TABLE *t, u8 *frame, u32 length, u32 extra, u32 now, WIN_PATCH *p);
void win_restore(u8 *frame, const WIN_PATCH *p);

/* A frame for xnet. 1: kept here (copied); 0: the caller hands it up now. */
u32 win_in(WIN_TABLE *t, const u8 *frame, u32 length, u32 extra, u32 now);

/* The oldest held frame of a connection if it fits now (`force`: whether or not), its
 * slot or WIN_NONE. The caller hands up t->pool + slot * WIN_FRAME, t->length[slot] bytes,
 * then gives the slot back with win_free. */
u32 win_take(WIN_TABLE *t, WIN_FLOW *f, u32 force);
void win_free(WIN_TABLE *t, u32 slot);

/* Once a tick: counts the connections receiving (t->busy), and returns one whose held
 * frames have waited WIN_PROBE ticks with the edge still, or 0. */
WIN_FLOW *win_stale(WIN_TABLE *t, u32 now);

/* --- the plugin's ----------------------------------------------------------- */

typedef struct {
    u32 opened;                 /* connections taken on */
    u32 not_taken;              /* their place was another's */
    u32 patched;                /* segments out whose window was changed */
    u32 held;                   /* segments kept */
    u32 released;               /* handed up when the edge had moved */
    u32 probed;                 /* handed up because it had not */
    u32 no_room;                /* beyond the edge and not kept: pool or `extra` full */
    u32 dropped;                /* held ones of a connection that ended or was reset */
    u32 held_now, held_most;
} WIN_STATS;

extern volatile WIN_STATS net_win_stats;
extern volatile u32 net_win_extra_kb;   /* 0: off */

void window_init(void);                 /* thread: the ini (`rx_window_kb =`), the pool */
void window_tx(u8 *frame, u32 length, WIN_PATCH *p);    /* net_tx, before the frame goes */
u32  window_rx(const u8 *frame, u32 length);            /* 1: kept, not for xnet yet */
void window_tick(void);

#endif /* XE_WINDOW_H */
