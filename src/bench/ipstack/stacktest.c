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

/* --- Winsock's overlapped calls, through xam's exports (hooked: winsock/overlap.c) -----
 * A TCP echo on 5310 the way a game would write it: WSAEventSelect(FD_ACCEPT) on the
 * listener and a wait on its event, then for each piece WSARecv with an OVERLAPPED,
 * WSAGetOverlappedResult, WSASend with an OVERLAPPED, WSAGetOverlappedResult.
 * `xectl.py cmd 31` starts it; the PC sends and reads back (scratch: echo.py). */
extern u32 NetDll_socket(u32 caller, int family, int type, int protocol);
extern int NetDll_bind(u32 caller, u32 s, const void *address, int length);
extern int NetDll_listen(u32 caller, u32 s, int backlog);
extern u32 NetDll_accept(u32 caller, u32 s, void *address, int *length);
extern int NetDll_closesocket(u32 caller, u32 s);
extern int NetDll_WSARecv(u32 caller, u32 s, void *bufs, u32 count, u32 *received, u32 *flags,
                          void *ov, void *completion);
extern int NetDll_WSASend(u32 caller, u32 s, void *bufs, u32 count, u32 *sent, u32 flags,
                          void *ov, void *completion);
extern int NetDll_WSAGetOverlappedResult(u32 caller, u32 s, void *ov, u32 *transferred, u32 wait,
                                         u32 *flags);
extern int NetDll_WSAEventSelect(u32 caller, u32 s, void *event, u32 events);
extern s32 NtCreateEvent(void **handle, void *attributes, u32 type, u32 initial);
extern s32 NtWaitForSingleObjectEx(void *handle, u32 mode, u32 alertable, s64 *timeout);
extern s32 NtClearEvent(void *handle);

volatile u32 overlap_test[8];       /* accepts, pieces, bytes, failures, last step failed, ... */
static u8 echo_buf[16384];

static void echo(void *arg)
{
    struct { u16 family, port; u32 address; u8 zero[8]; } a;
    struct { u32 length; u8 *buffer; } b;
    struct { u32 internal, high, offset, offset_high; void *event; } ov;
    void *listen_event = 0, *io_event = 0;
    u32 s, c, n, flags, got;
    int len;

    (void)arg;
    NtCreateEvent(&listen_event, 0, 0, 0);
    NtCreateEvent(&io_event, 0, 0, 0);
    s = NetDll_socket(1, 2, 1, 6);
    memset(&a, 0, sizeof(a));
    a.family = 2;
    a.port = 5310;
    if (s == 0xFFFFFFFFu || NetDll_bind(1, s, &a, sizeof(a)) < 0 || NetDll_listen(1, s, 4) < 0 ||
        NetDll_WSAEventSelect(1, s, listen_event, 0x08) < 0) {
        overlap_test[4] = 1;
        return;
    }
    for (;;) {
        NtWaitForSingleObjectEx(listen_event, 0, 0, 0);
        NtClearEvent(listen_event);
        len = sizeof(a);
        c = NetDll_accept(1, s, &a, &len);
        if (c == 0xFFFFFFFFu)
            continue;                               /* the socket waits no more: none yet */
        overlap_test[0]++;
        for (;;) {
            b.length = sizeof(echo_buf);
            b.buffer = echo_buf;
            memset(&ov, 0, sizeof(ov));
            ov.event = io_event;
            NtClearEvent(io_event);
            flags = 0;
            n = 0;
            NetDll_WSARecv(1, c, &b, 1, &n, &flags, &ov, 0);
            if (!NetDll_WSAGetOverlappedResult(1, c, &ov, &got, 1, &flags) || !got) {
                if (ov.internal)
                    overlap_test[3]++;
                break;                              /* the peer closed, or it failed */
            }
            overlap_test[1]++;
            overlap_test[2] += got;
            b.length = got;
            memset(&ov, 0, sizeof(ov));
            ov.event = io_event;
            NtClearEvent(io_event);
            NetDll_WSASend(1, c, &b, 1, &n, 0, &ov, 0);
            if (!NetDll_WSAGetOverlappedResult(1, c, &ov, &got, 1, &flags) || got != b.length) {
                overlap_test[3]++;
                overlap_test[4] = 2;
                break;
            }
        }
        NetDll_closesocket(1, c);
    }
}

u32 stacktest_overlap_start(void)
{
    static volatile u32 once;

    if (__sync_lock_test_and_set(&once, 1))
        return 1;
    return sys_thread_new("echo", echo, 0, 0x8000, 20) ? 0 : 2;
}

u32 stacktest_start(void)
{
    if (__sync_lock_test_and_set(&stacktest_stats.started, 1))
        return 1;
    sys_thread_new("source", listener, (void *)SOURCE_PORT, 0x8000, 20);
    sys_thread_new("sink", listener, (void *)SINK_PORT, 0x8000, 20);
    return 0;
}
