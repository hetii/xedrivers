/* The tuning tools behind src/core/bench.h: the commands only they answer, the
 * worker's tick, the standalone mode. Built when `make bench=...` names any tool;
 * each tool is compiled in only if named (XE_BENCH_<TOOL>, XE_PROF, XE_TCP).
 */

#include "bench.h"
#include "config.h"
#include "prof.h"
#if XE_BENCH_HISTORY
#include "history.h"
#endif
#if XE_BENCH_STACK
#include "stack.h"
#endif
#if XE_BENCH_DEBUG
#include "debug.h"
#endif
#if XE_TCP
#include "tcp.h"
#endif

#define CMD_BLAST       4u  /* [1] count, [2] payload bytes, [3] UDP port (stack) */
#define CMD_TCP_START  14u  /* start the throughput server (tcp, xnet mode) */
#define CMD_THREAD_PRIO 15u /* [1] thread id (xerunner console list-threads), [2] prio */
#define CMD_PEEK       16u  /* [1] address, [2] 1 = little-endian (MMIO) -> word */
#define CMD_POKE       17u  /* [1] address, [2] 1 = little-endian, [3] word */
#define CMD_CONFIG     18u  /* [1] category, [2] setting -> size, bytes in
                             * debug_config_value */

void bench_init(void)
{
    prof_init();
}

void bench_tick(u32 tick)
{
    PROF_STAMP();
#if XE_BENCH_HISTORY
    history_tick(tick);
#endif
    (void)tick;
}

u32 bench_command(u32 what, volatile u32 *args)
{
    switch (what) {
#if XE_BENCH_STACK
    case CMD_BLAST:
        args[5] = stack_blast(args[1], args[2], args[3]);
        return 1;
#endif
#if XE_BENCH_DEBUG
    case CMD_THREAD_PRIO:
        args[5] = debug_thread_priority(args[1], args[2]);
        return 1;
    case CMD_PEEK:
        args[5] = debug_peek(args[1], args[2]);
        return 1;
    case CMD_CONFIG:
        args[5] = debug_config(args[1], args[2]);
        return 1;
    case CMD_POKE:
        args[5] = debug_poke(args[1], args[2], args[3]);
        return 1;
#endif
#if XE_TCP
    case CMD_TCP_START:
        args[5] = tcp_start();
        return 1;
#endif
    default:
        (void)args;
        return 0;
    }
}

BENCH_RX bench_mode(u32 mode)
{
#if XE_BENCH_STACK
    if (mode == XE_MODE_STANDALONE) {
        stack_ip = xe_config.ip;
        return stack_input;
    }
#endif
    (void)mode;
    return 0;
}
