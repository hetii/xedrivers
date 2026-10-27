/* Putting a block ack's frames back in order, for any Wi-Fi card whose firmware
 * hands an A-MPDU's frames up as they decode; see reorder.c.
 *
 * The driver owns everything about its chip -- which frames need ordering, how a
 * received packet becomes an Ethernet frame, where it runs -- and calls in from one
 * context only: its receive path, and a timeout of its own scheduling run in that
 * same context. This module holds the windows and hands frames back through
 * `deliver`, in order.
 */

#ifndef XE_WL_REORDER_H
#define XE_WL_REORDER_H

#include "kernel.h"

#define WL_REORDER_TIDS    8
#define WL_REORDER_WINDOW  64       /* sequence numbers per TID, as AICWF_REORDER_WINSIZE */
#define WL_REORDER_SLOTS   64       /* frames held at once, across all TIDs */
#define WL_REORDER_FRAME   1536     /* an Ethernet frame at MTU, rounded up */
#define WL_REORDER_POOL    (WL_REORDER_SLOTS * WL_REORDER_FRAME)
#define WL_REORDER_TICKS   2        /* 20 ms worker ticks a hole may hold the others
                                     * (Linux waits 50 ms) */
#define WL_REORDER_RESYNC  32       /* that many behind in a row: the sender restarted */
#define WL_REORDER_NONE    0xFF

/* Counted where the driver keeps its statistics; this order is what xectl reads. */
typedef struct {
    u32 held;               /* frames kept back until the ones before them came */
    u32 late;               /* behind the window, handed up as they came */
    u32 jumps;              /* past the window's end: the window moved up to it */
    u32 timeouts;           /* holes skipped because nothing filled them */
    u32 full;               /* no room to hold one: everything let go at once */
    u32 resyncs;            /* the window moved to a sender that started again */
    u32 ended;              /* a TID's frames stopped asking to be ordered */
} WL_REORDER_STATS;

typedef void (*WL_DELIVER)(void *context, u8 *frame, u32 length);

typedef struct {
    u16 head;               /* the next sequence number to hand up */
    u8  active;
    volatile u8 stalled;    /* worker ticks with frames held and no progress */
    u8  held;
    u8  late_run;           /* frames in a row behind the window */
    u8  slot[WL_REORDER_WINDOW];    /* by sequence number mod 64: pool slot or NONE */
    u16 length[WL_REORDER_WINDOW];
} WL_REORDER_TID;

typedef struct {
    WL_REORDER_TID tid[WL_REORDER_TIDS];
    u8 *pool;               /* WL_REORDER_POOL bytes, the driver's; 0: hand all up */
    u8 free[WL_REORDER_SLOTS];
    u32 free_count;
    WL_DELIVER deliver;
    void *context;
    volatile WL_REORDER_STATS *st;
} WL_REORDER;

/* Once, before the first frame: the pool may be 0, and then nothing is held. */
void wl_reorder_init(WL_REORDER *r, u8 *pool, WL_DELIVER deliver, void *context,
                     volatile WL_REORDER_STATS *st);

/* A frame the firmware says needs ordering, as an Ethernet frame: handed up now,
 * with what it completes, or copied aside until the ones before it come. */
void wl_reorder_in(WL_REORDER *r, u32 tid, u32 seq, u8 *frame, u32 length);

/* A frame of `tid` the firmware no longer asks to be ordered: the block-ack session
 * is over, so what was held goes up first (the driver then hands that frame up). */
void wl_reorder_end(WL_REORDER *r, u32 tid);

/* A new association: the access point numbers afresh, so the windows start again. */
void wl_reorder_reset(WL_REORDER *r);

/* From the worker, every tick: 1 when a hole has held frames back long enough and
 * wl_reorder_expire should run -- in the receive path's context, never here. */
u32 wl_reorder_tick(WL_REORDER *r);
void wl_reorder_expire(WL_REORDER *r);

#endif /* XE_WL_REORDER_H */
