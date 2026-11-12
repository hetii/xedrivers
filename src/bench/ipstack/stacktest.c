/* See stacktest.h. lwIP's socket API, a thread a connection. */
#include "lwip/opt.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <string.h>

#include "stacktest.h"
#include "ipstack.h"

#define SOURCE_PORT 5301
#define SINK_PORT   5302
#define BLOCK       65536

volatile STACKTEST_STATS stacktest_stats;

static u8 zeros[BLOCK], taken[BLOCK];   /* what is taken is thrown away: one buffer for all */
static KSPIN_LOCK lock;

static void add(volatile u32 *hi, volatile u32 *lo, u32 n)
{
    u32 irql = KfAcquireSpinLock(&lock), was = *lo;

    *lo = was + n;
    if (was + n < was)
        *hi = *hi + 1;
    KfReleaseSpinLock(&lock, irql);
}

static void sink(void *arg)
{
    int c = (int)arg, n;

    while ((n = lwip_recv(c, taken, BLOCK, 0)) > 0)
        add(&stacktest_stats.recv_hi, &stacktest_stats.recv_lo, (u32)n);
    lwip_close(c);
    __sync_fetch_and_sub(&stacktest_stats.active, 1);
}

static void source(void *arg)
{
    int c = (int)arg, n;

    while ((n = lwip_send(c, zeros, BLOCK, 0)) > 0)
        add(&stacktest_stats.sent_hi, &stacktest_stats.sent_lo, (u32)n);
    lwip_close(c);
    __sync_fetch_and_sub(&stacktest_stats.active, 1);
}

static void listener(void *arg)
{
    u32 port = (u32)arg;
    struct sockaddr_in at;
    int s = lwip_socket(AF_INET, SOCK_STREAM, 0), c;

    memset(&at, 0, sizeof(at));
    at.sin_len = sizeof(at);
    at.sin_family = AF_INET;
    at.sin_port = lwip_htons((u16_t)port);
    ipstack_port(port, 1);
    if (s < 0 || lwip_bind(s, (struct sockaddr *)&at, sizeof(at)) < 0 || lwip_listen(s, 16) < 0) {
        stacktest_stats.errors++;
        return;
    }
    for (;;) {
        c = lwip_accept(s, 0, 0);
        if (c < 0) {
            stacktest_stats.errors++;
            sys_msleep(100);
            continue;
        }
        stacktest_stats.accepts++;
        __sync_fetch_and_add(&stacktest_stats.active, 1);
        if (!sys_thread_new("conn", port == SINK_PORT ? sink : source, (void *)c, 0x8000, 20)) {
            stacktest_stats.errors++;
            lwip_close(c);
            __sync_fetch_and_sub(&stacktest_stats.active, 1);
        }
    }
}

u32 stacktest_start(void)
{
    if (__sync_lock_test_and_set(&stacktest_stats.started, 1))
        return 1;
    sys_thread_new("source", listener, (void *)SOURCE_PORT, 0x8000, 20);
    sys_thread_new("sink", listener, (void *)SINK_PORT, 0x8000, 20);
    return 0;
}
