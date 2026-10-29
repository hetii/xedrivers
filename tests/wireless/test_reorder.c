/* Native test of the block-ack windows (src/wireless/reorder.c) on their own,
 * without any card: frames go in by TID and sequence number, and what comes out of
 * `deliver` -- and in which order -- is what is checked. The AIC8800's test covers
 * the same windows behind its own receive path. */

#include <stdio.h>
#include <string.h>

#include "reorder.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                      failures++; } } while (0)

static u8 pool[WL_REORDER_POOL];
static volatile WL_REORDER_STATS st;
static WL_REORDER r;
static u32 out[256], outs;

/* Each frame carries its sequence number in its first two bytes. */
static void deliver(void *context, u8 *frame, u32 length)
{
    (void)context;
    CHECK(length == 64);
    if (outs < 256)
        out[outs++] = (u32)(frame[0] | frame[1] << 8);
}

static void in(u32 tid, u32 seq)
{
    u8 f[64];

    memset(f, 0, sizeof(f));
    f[0] = (u8)seq;
    f[1] = (u8)(seq >> 8);
    wl_reorder_in(&r, tid, seq, f, sizeof(f));
}

static int order(const u32 *want, u32 n)
{
    u32 i;

    if (outs != n)
        return 0;
    for (i = 0; i < n; i++)
        if (out[i] != want[i])
            return 0;
    return 1;
}

int main(void)
{
    u32 i;

    wl_reorder_init(&r, pool, deliver, 0, &st);

    /* In order: straight through, nothing held. */
    for (i = 10; i < 14; i++)
        in(0, i);
    { static const u32 w[] = { 10, 11, 12, 13 }; CHECK(order(w, 4)); }
    CHECK(st.held == 0);

    /* A retried frame late: the two behind it wait for it, then all three go. */
    outs = 0;
    in(0, 15);
    in(0, 16);
    CHECK(outs == 0 && st.held == 2);
    in(0, 14);
    { static const u32 w[] = { 14, 15, 16 }; CHECK(order(w, 3)); }

    /* Behind the window: handed up as it came, counted. */
    outs = 0;
    in(0, 12);
    CHECK(outs == 1 && out[0] == 12 && st.late == 1);

    /* TIDs are separate windows. */
    outs = 0;
    in(5, 1000);
    in(0, 17);
    CHECK(outs == 2 && st.held == 2);

    /* A hole nothing fills: after the ticks, the timeout lets the rest go. */
    outs = 0;
    in(0, 19);
    CHECK(outs == 0);
    for (i = 1; i < WL_REORDER_TICKS; i++)
        CHECK(!wl_reorder_tick(&r));
    CHECK(wl_reorder_tick(&r));
    wl_reorder_expire(&r);
    CHECK(outs == 1 && out[0] == 19 && st.timeouts == 1);
    CHECK(r.free_count == WL_REORDER_SLOTS);

    /* Far past the window's end: it moves so that frame is its last. */
    outs = 0;
    in(0, 22);                          /* held: 20 and 21 missing */
    in(0, 200);                         /* head to 137: 22 goes, 200 is held */
    CHECK(outs == 1 && out[0] == 22 && st.jumps == 1);
    CHECK(r.tid[0].head == 137 && r.tid[0].held == 1);

    /* The session ends: what was held goes up. */
    outs = 0;
    wl_reorder_end(&r, 0);
    CHECK(outs == 1 && out[0] == 200 && st.ended == 1 && !r.tid[0].active);

    /* A new association numbers from scratch. */
    wl_reorder_reset(&r);
    outs = 0;
    in(0, 3);
    in(0, 4);
    { static const u32 w[] = { 3, 4 }; CHECK(order(w, 2)); }

    /* No room left: that TID's held frames go at once, then the frame itself.
     * TID 0 holds 63 behind a hole, TID 1 the last free slot and then wants more. */
    wl_reorder_reset(&r);
    in(0, 0);
    for (i = 2; i <= WL_REORDER_WINDOW; i++)
        in(0, i);
    in(1, 0);
    in(1, 2);
    CHECK(r.free_count == 0);
    outs = 0;
    in(1, 3);
    { static const u32 w[] = { 2, 3 }; CHECK(order(w, 2)); }
    CHECK(st.full == 1 && r.tid[0].held == WL_REORDER_WINDOW - 1);

    /* drop_late (a SoftMAC card): behind the window is a duplicate and goes, unless
     * the window gave its number up -- an AP sending past the window, the holes'
     * frames coming just after. */
    wl_reorder_init(&r, pool, deliver, 0, &st);
    r.drop_late = 1;
    st.late = st.rescued = 0;
    for (i = 1430; i < 1440; i++)
        if (i != 1432 && i != 1435)
            in(0, i);                   /* 1430, 1431 go; 1433.. wait for 1432 */
    outs = 0;
    in(0, 1503);                        /* head to 1440: 1432 and 1435 given up */
    CHECK(st.jumps >= 1 && r.tid[0].head == 1440);
    { static const u32 w[] = { 1433, 1434, 1436, 1437, 1438, 1439 }; CHECK(order(w, 6)); }
    outs = 0;
    in(0, 1432);                        /* given up on: handed up after all */
    in(0, 1435);
    in(0, 1432);                        /* now a duplicate */
    in(0, 1431);                        /* delivered long ago: a duplicate */
    { static const u32 w[] = { 1432, 1435 }; CHECK(order(w, 2)); }
    CHECK(st.rescued == 2 && st.late == 4);

    /* No pool: everything handed up as it comes. */
    wl_reorder_init(&r, 0, deliver, 0, &st);
    outs = 0;
    in(0, 9);
    in(0, 7);
    { static const u32 w[] = { 9, 7 }; CHECK(order(w, 2)); }

    if (failures)
        return 1;
    printf("all reorder tests passed\n");
    return 0;
}
