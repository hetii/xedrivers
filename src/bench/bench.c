/* The tuning tools behind src/core/bench.h: the commands only they answer, the
 * worker's tick, the standalone mode. Built with BENCH=1 only.
 */

#include "bench.h"
#include "config.h"
#include "prof.h"
#include "history.h"
#include "stack.h"
#include "debug.h"
#if XE_TCP
#include "tcp.h"
#endif

#define CMD_BLAST       4u  /* [1] count, [2] payload bytes, [3] UDP port */
#define CMD_TCP_START  14u  /* start the throughput server (xnet mode, TCP=1 builds) */
#define CMD_THREAD_PRIO 15u /* [1] thread id (xerunner console list-threads), [2] prio */
#define CMD_PEEK       16u  /* [1] address, [2] 1 = little-endian (MMIO) -> word */
#define CMD_POKE       17u  /* [1] address, [2] 1 = little-endian, [3] word */

void bench_init(void)
{
    prof_init();
}

void bench_tick(u32 tick)
{
    PROF_STAMP();
    history_tick(tick);
}

u32 bench_command(u32 what, volatile u32 *args)
{
    switch (what) {
    case CMD_BLAST:
        args[5] = stack_blast(args[1], args[2], args[3]);
        return 1;
    case CMD_THREAD_PRIO:
        args[5] = debug_thread_priority(args[1], args[2]);
        return 1;
    case CMD_PEEK:
        args[5] = debug_peek(args[1], args[2]);
        return 1;
    case CMD_POKE:
        args[5] = debug_poke(args[1], args[2], args[3]);
        return 1;
#if XE_TCP
    case CMD_TCP_START:
        args[5] = tcp_start();
        return 1;
#endif
    default:
        return 0;
    }
}

NET_RX bench_mode(u32 mode)
{
    if (mode != XE_MODE_STANDALONE)
        return 0;
    stack_ip = xe_config.ip;
    return stack_input;
}
