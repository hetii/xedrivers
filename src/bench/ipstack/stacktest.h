/* Throughput servers on the console's own stack (make stack=1 bench=ipstack), the twins of
 * src/bench/tcp on xnet: port 5301 sends, 5302 takes; `tools/xectl.py bench tcp --stack`. */
#ifndef XE_BENCH_STACKTEST_H
#define XE_BENCH_STACKTEST_H

#include "kernel.h"

typedef struct {
    u32 recv_hi, recv_lo;       /* bytes taken on 5302 */
    u32 sent_hi, sent_lo;       /* bytes sent on 5301 */
    u32 started, accepts, active, errors;
} STACKTEST_STATS;

extern volatile STACKTEST_STATS stacktest_stats;

u32 stacktest_start(void);      /* 0 when started now, 1 if it was */

#endif
