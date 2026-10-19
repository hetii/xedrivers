/* Where the core lets the tuning tools in (src/bench/), and nothing more.
 *
 * Everything that exists to measure and tune -- the flight recorder, the
 * standalone responder with its UDP generator, the stage timers of PROF=1, the
 * register and thread-priority commands, the TCP server of TCP=1 -- lives under
 * src/bench/ and is built only with BENCH=1 (the default while developing;
 * `make RELEASE=1` leaves it out). The core reaches it through these four calls;
 * without BENCH every one is an empty inline the compiler drops, so a release build
 * carries none of the code, data or cycles.
 */

#ifndef XE_BENCH_H
#define XE_BENCH_H

#include "kernel.h"
#include "net.h"

#ifndef XE_BENCH
#define XE_BENCH 0
#endif

#if XE_BENCH
/* Once, from the worker before its loop. */
void   bench_init(void);

/* Every worker tick (20 ms). */
void   bench_tick(u32 tick);

/* A host command the core does not know, before the driver is asked: 1 if handled,
 * the result in args[5]. */
u32    bench_command(u32 what, volatile u32 *args);

/* The receiver for a mode only the bench has (XE_MODE_STANDALONE), set up for it;
 * 0 for any other mode. */
NET_RX bench_mode(u32 mode);
#else
static inline void   bench_init(void) { }
static inline void   bench_tick(u32 tick) { (void)tick; }
static inline u32    bench_command(u32 what, volatile u32 *args)
{
    (void)what;
    (void)args;
    return 0;
}
static inline NET_RX bench_mode(u32 mode) { (void)mode; return 0; }
#endif

#endif /* XE_BENCH_H */
