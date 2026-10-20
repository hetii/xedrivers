/* Where the time of a frame goes, measured with the time base. `make bench=prof` only:
 * a plain build compiles every macro here to nothing.
 *
 * Each slot sums the ticks spent inside one stage, the calls and the bytes they
 * handled. tests/xectl.py prof takes two snapshots and turns the difference into
 * microseconds a call, nanoseconds a byte and the share of wall time; `stamp` is
 * the time base when the worker last ran, so the wall time comes from the console's
 * own clock. Stages nest -- the transmit completion includes the copies and
 * submits it drains -- and are reported side by side, not summed.
 *
 * Slots are updated without a lock from several processors; a lost count now and
 * then does not move an average.
 */

#ifndef XE_PROF_H
#define XE_PROF_H

#include "kernel.h"

#ifndef XE_PROF
#define XE_PROF 0
#endif

typedef struct {
    u32 ticks_hi, ticks_lo;
    u32 bytes_hi, bytes_lo;
    u32 calls;
} PROF_SLOT;

typedef struct {
    u32 enabled;
    u32 freq;               /* KeQueryPerformanceFrequency, ticks a second */
    u32 stamp;              /* time base, low word, written by the worker */
    PROF_SLOT copy;         /* the frame copied into its transfer buffer */
    PROF_SLOT submit;       /* UsbdQueueAsyncTransfer, any endpoint */
    PROF_SLOT tx_done;      /* a transmit completion, with what it drains */
    PROF_SLOT rx_done;      /* a receive completion, with the frames handed up */
    PROF_SLOT rx_up;        /* the receiver of one frame: xnet or the stack */
} NET_PROF;

extern volatile NET_PROF net_prof;

#if XE_PROF && defined(__powerpc__)
static inline u32 prof_tb(void)
{
    u32 t;
    __asm__ volatile("mftb %0" : "=r"(t));
    return t;
}

static inline void prof_add(volatile PROF_SLOT *slot, u32 since, u32 bytes)
{
    u32 ticks = prof_tb() - since, lo;

    lo = slot->ticks_lo + ticks;
    if (lo < ticks)
        slot->ticks_hi++;
    slot->ticks_lo = lo;
    lo = slot->bytes_lo + bytes;
    if (lo < bytes)
        slot->bytes_hi++;
    slot->bytes_lo = lo;
    slot->calls++;
}

#define PROF_START(v)          u32 v = prof_tb()
#define PROF_END(slot, v, n)   prof_add(&net_prof.slot, v, n)
#define PROF_STAMP()           (net_prof.stamp = prof_tb())
#else
#define PROF_START(v)          do { } while (0)
#define PROF_END(slot, v, n)   do { } while (0)
#define PROF_STAMP()           do { } while (0)
#endif

#if XE_PROF
void prof_init(void);
#else
static inline void prof_init(void) { }
#endif

#endif /* XE_PROF_H */
