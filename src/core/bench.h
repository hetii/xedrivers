/* Where the core lets the tuning tools in (src/bench/), and nothing more.
 *
 * Everything that exists to measure and tune -- the flight recorder, the
 * standalone responder with its UDP generator, the stage timers, the register and
 * thread-priority commands, the TCP server -- lives under src/bench/<tool>/ and is
 * built only when named: `make bench=history,stack`, `make bench=all`. A plain
 * `make` is the plugin for people and has none of it. The core reaches it through
 * these calls; without any tool every one is an empty inline the compiler
 * drops, so that build carries none of the code, data or cycles.
 */

#ifndef XE_BENCH_H
#define XE_BENCH_H

#include "kernel.h"

/* A frame receiver, as the network layer's NET_RX; spelt out so that the core does
 * not depend on the layer. */
typedef void (*BENCH_RX)(const u8 *frame, u32 length);

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
BENCH_RX bench_mode(u32 mode);

/* Non-zero while the console is to be told that no cable is plugged in -- neither
 * the card nor the onboard port -- so that the dashboard turns to a wireless card
 * (the omni probe). Asked on every link poll. */
u32    bench_wired_hidden(void);

/* A short mark on the console's UART (bench=debug), for what a hang leaves no
 * other trace of: read with `xerunner flash read-uart`. Any IRQL. */
void   bench_trace(const char *mark);


/* Something that changes the link, for the recorder that survives a hang
 * (bench=trace): a code and three words. Any IRQL. */
void   bench_event(u32 code, u32 a, u32 b, u32 c);

/* Every frame xnet sends through the card, and every one handed up to it. */
void   bench_frame_out(const u8 *frame, u32 length);
void   bench_frame_in(const u8 *frame, u32 length);
#else
static inline void   bench_init(void) { }
static inline void   bench_tick(u32 tick) { (void)tick; }
static inline u32    bench_command(u32 what, volatile u32 *args)
{
    (void)what;
    (void)args;
    return 0;
}
static inline BENCH_RX bench_mode(u32 mode) { (void)mode; return 0; }
static inline u32    bench_wired_hidden(void) { return 0; }
static inline void   bench_trace(const char *mark) { (void)mark; }
static inline void   bench_event(u32 code, u32 a, u32 b, u32 c)
{ (void)code; (void)a; (void)b; (void)c; }
static inline void   bench_frame_out(const u8 *frame, u32 length) { (void)frame; (void)length; }
static inline void   bench_frame_in(const u8 *frame, u32 length) { (void)frame; (void)length; }
#endif

#endif /* XE_BENCH_H */
