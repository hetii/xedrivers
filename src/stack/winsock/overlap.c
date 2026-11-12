/* Overlapped sends and receives, and WSAEventSelect.
 *
 * An overlapped call (an OVERLAPPED with an event, as the console's Winsock has it -- no
 * completion routines) is tried at once without waiting; what cannot be done now is kept as
 * a job, and one thread watches every job's socket and every socket given to WSAEventSelect
 * with lwIP's select, finishes a job when its socket is ready, and sets the caller's event.
 * The OVERLAPPED says how it went the way Windows' does: Internal STATUS_PENDING (0x103)
 * while it runs, then 0 or the Winsock code, InternalHigh the bytes moved.
 *
 * WSAEventSelect's events are kept level-triggered for reading (the event is set while
 * there is something to read, accept or a close to see) and once for writing (after the
 * call, and again only after a send that would have waited), as Winsock re-enables them.
 */
#include "lwip/opt.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <string.h>

#include "winsock.h"
#include "ipstack.h"

#if XE_STACK

extern s32 NtSetEvent(void *event, s32 *previous);

#define STATUS_PENDING      0x103u
#define WSA_IO_PENDING      997
#define WSA_IO_INCOMPLETE   996
#define WSA_OPERATION_ABORTED 995
#define JOBS                32u
#define WATCHED             32u
#define BUFS                4u

#define FD_READ             0x01u
#define FD_WRITE            0x02u
#define FD_OOB              0x04u
#define FD_ACCEPT           0x08u
#define FD_CONNECT          0x10u
#define FD_CLOSE            0x20u

typedef struct {
    u32 internal, internal_high, offset, offset_high;
    void *event;
} WS_OVERLAPPED;

typedef struct {
    volatile u32 used;
    u32 send;                       /* 1 send, 0 receive */
    s32 fd;
    u32 handle;
    WS_OVERLAPPED *ov;
    WS_BUF buf[BUFS];
    u32 count;
    u32 sent;                       /* a send: bytes gone so far; it ends when all are */
    u32 flags;
    u32 has_to;
    struct sockaddr_in to;
    WS_SOCKADDR_IN *from;           /* the caller's, for a receive: filled when it is done */
    s32 *fromlength;
} JOB;

static JOB job[JOBS];
static struct {
    u32 handle;                     /* 0: free */
    s32 fd;
    void *event;
    u32 events;
    u32 write_armed;                /* FD_WRITE is to be told when the socket can be written */
} watch[WATCHED];
static KSPIN_LOCK lock;
static volatile u32 started_thread;

/* One try, without waiting: bytes moved, or < 0 with lwIP's errno set. A send on a stream
 * ends only when all of it has gone, as Winsock's does: what lwIP takes is counted, and
 * until the last byte the job waits on (EWOULDBLOCK). */
static int attempt(JOB *j)
{
    struct sockaddr_in a;
    socklen_t n = sizeof(a);
    u32 i;
    int r;

    if (j->send) {
        u32 total = 0, at = j->sent;

        if (j->has_to)
            return (int)lwip_sendto(j->fd, j->buf[0].buffer, j->buf[0].length, MSG_DONTWAIT,
                                    (struct sockaddr *)&j->to, sizeof(j->to));
        for (i = 0; i < j->count; i++)
            total += j->buf[i].length;
        for (i = 0; i < j->count && j->sent < total; i++) {
            if (at >= j->buf[i].length) {
                at -= j->buf[i].length;
                continue;
            }
            r = (int)lwip_send(j->fd, j->buf[i].buffer + at, j->buf[i].length - at,
                               MSG_DONTWAIT);
            if (r < 0) {
                if (j->sent && (*ipstack_errno() == EWOULDBLOCK || *ipstack_errno() == EAGAIN))
                    return -1;              /* some went: the rest when there is room */
                return r;
            }
            j->sent += (u32)r;
            if ((u32)r < j->buf[i].length - at) {
                *ipstack_errno() = EWOULDBLOCK;
                return -1;
            }
            at = 0;
        }
        return (int)j->sent;
    }
    r = (int)lwip_recvfrom(j->fd, j->buf[0].buffer, j->buf[0].length, MSG_DONTWAIT,
                           (struct sockaddr *)&a, &n);
    if (r >= 0 && j->from && j->fromlength && *j->fromlength >= (s32)sizeof(WS_SOCKADDR_IN)) {
        ws_from_lwip(&a, j->from);
        *j->fromlength = sizeof(WS_SOCKADDR_IN);
    }
    return r;
}

static void finish(JOB *j, s32 status, u32 bytes)
{
    j->ov->internal_high = bytes;
    mem_sync();
    j->ov->internal = (u32)status;
    if (j->ov->event)
        NtSetEvent(j->ov->event, 0);
}

static int would_wait(void)
{
    return *ipstack_errno() == EWOULDBLOCK || *ipstack_errno() == EAGAIN;
}

static void watcher(void *arg);

static void start_watcher(void)
{
    if (!__sync_lock_test_and_set(&started_thread, 1))
        sys_thread_new("overlap", watcher, 0, 0x4000, 20);
}

/* An overlapped call: done now (0, the event set), or kept (WSA_IO_PENDING). */
static s32 overlapped(JOB *t, WS_OVERLAPPED *ov, u32 *moved)
{
    u32 i;
    int r;

    if (!ov)
        return ws_fail(WSAEINVAL);
    ov->internal = STATUS_PENDING;
    ov->internal_high = 0;
    r = attempt(t);
    if (r >= 0) {
        t->ov = ov;
        finish(t, 0, (u32)r);
        if (moved)
            *moved = (u32)r;
        return 0;
    }
    if (!would_wait()) {
        ov->internal = (u32)WSAECONNRESET;
        return ws_fail_errno();
    }
    for (i = 0; i < JOBS; i++)
        if (!__sync_lock_test_and_set(&job[i].used, 1))
            break;
    if (i == JOBS) {
        ov->internal = (u32)WSAENOBUFS;
        return ws_fail(WSAENOBUFS);
    }
    t->ov = ov;
    t->used = 1;
    job[i] = *t;                            /* claimed (1): the watcher waits for 2 */
    mem_sync();
    job[i].used = 2;
    start_watcher();
    return ws_fail(WSA_IO_PENDING);
}

static void fill_job(JOB *t, u32 s, s32 fd, const WS_BUF *bufs, u32 count, u32 send)
{
    u32 i;

    memset(t, 0, sizeof(*t));
    t->send = send;
    t->fd = fd;
    t->handle = s;
    t->count = count > BUFS ? BUFS : count;
    for (i = 0; i < t->count; i++)
        t->buf[i] = bufs[i];
}

/* calls.c hands an overlapped WSARecv / WSASend here. */
s32 ws_overlapped_recv(u32 s, s32 fd, WS_BUF *bufs, u32 count, u32 *received, void *ov)
{
    JOB t;

    if (!bufs || !count)
        return ws_fail(WSAEINVAL);
    fill_job(&t, s, fd, bufs, 1, 0);         /* one buffer: a datagram or a stream piece */
    return overlapped(&t, (WS_OVERLAPPED *)ov, received);
}

s32 ws_overlapped_send(u32 s, s32 fd, const WS_BUF *bufs, u32 count, u32 *sent, void *ov)
{
    JOB t;

    if (!bufs || !count)
        return ws_fail(WSAEINVAL);
    fill_job(&t, s, fd, bufs, count, 1);
    return overlapped(&t, (WS_OVERLAPPED *)ov, sent);
}

/* NetDll_WSARecvFrom (21): ten arguments, the last two read off the caller's stack by
 * hook.c's stub. */
s32 ws_WSARecvFrom(u32 caller, u32 s, WS_BUF *bufs, u32 count, u32 *received, u32 *flags,
                   WS_SOCKADDR_IN *from, s32 *fromlength, void *ov, void *completion)
{
    s32 fd = ws_fd(s);
    JOB t;

    (void)caller; (void)completion;
    if (fd < 0)
        return WS_ERROR;
    ws_stats.calls++;
    if (!bufs || !count)
        return ws_fail(WSAEINVAL);
    fill_job(&t, s, fd, bufs, 1, 0);
    t.from = from;
    t.fromlength = fromlength;
    if (flags)
        *flags = 0;
    if (ov)
        return overlapped(&t, (WS_OVERLAPPED *)ov, received);
    {
        struct sockaddr_in a;
        socklen_t n = sizeof(a);
        int r = (int)lwip_recvfrom(fd, bufs[0].buffer, bufs[0].length, 0,
                                   (struct sockaddr *)&a, &n);

        if (r < 0)
            return ws_fail_errno();
        if (from && fromlength && *fromlength >= (s32)sizeof(WS_SOCKADDR_IN)) {
            ws_from_lwip(&a, from);
            *fromlength = sizeof(WS_SOCKADDR_IN);
        }
        if (received)
            *received = (u32)r;
        return 0;
    }
}

/* NetDll_WSASendTo (25): ten arguments, likewise. */
s32 ws_WSASendTo(u32 caller, u32 s, WS_BUF *bufs, u32 count, u32 *sent, u32 flags,
                 const WS_SOCKADDR_IN *to, s32 tolength, void *ov, void *completion)
{
    s32 fd = ws_fd(s);
    JOB t;
    int r;

    (void)caller; (void)flags; (void)completion;
    if (fd < 0)
        return WS_ERROR;
    ws_stats.calls++;
    if (!bufs || !count)
        return ws_fail(WSAEINVAL);
    fill_job(&t, s, fd, bufs, 1, 1);
    if (to) {
        if (tolength < (s32)sizeof(WS_SOCKADDR_IN) - 8)
            return ws_fail(WSAEFAULT);
        ws_to_lwip(to, &t.to);
        t.has_to = 1;
    }
    if (ov)
        return overlapped(&t, (WS_OVERLAPPED *)ov, sent);
    r = t.has_to ? (int)lwip_sendto(fd, bufs[0].buffer, bufs[0].length, 0,
                                    (struct sockaddr *)&t.to, sizeof(t.to))
                 : (int)lwip_send(fd, bufs[0].buffer, bufs[0].length, 0);
    if (r < 0)
        return ws_fail_errno();
    if (sent)
        *sent = (u32)r;
    return 0;
}

/* NetDll_WSAGetOverlappedResult (16) */
s32 ws_WSAGetOverlappedResult(u32 caller, u32 s, WS_OVERLAPPED *ov, u32 *transferred,
                              u32 wait, u32 *flags)
{
    (void)caller; (void)s;
    if (!ov) {
        ws_set_error(WSAEINVAL);
        return 0;
    }
    while (ov->internal == STATUS_PENDING) {
        JOB *mine = 0;
        u32 i;

        if (!wait) {
            ws_set_error(WSA_IO_INCOMPLETE);
            return 0;                           /* FALSE */
        }
        /* the caller waits for it: it does the job itself, as soon as the socket is ready
         * (the watcher would see a job added while it waits only after its time-out) */
        for (i = 0; i < JOBS; i++)
            if (job[i].ov == ov && __sync_bool_compare_and_swap(&job[i].used, 2, 3)) {
                mine = &job[i];
                break;
            }
        if (!mine) {
            sys_msleep(1);                      /* the watcher has it now */
            continue;
        }
        for (;;) {
            fd_set set;
            struct timeval t;
            int got = attempt(mine);

            if (got >= 0 || !would_wait()) {
                finish(mine, got >= 0 ? 0 : WSAECONNRESET, got >= 0 ? (u32)got : 0);
                break;
            }
            FD_ZERO(&set);
            FD_SET(mine->fd, &set);
            t.tv_sec = 1;
            t.tv_usec = 0;
            lwip_select(mine->fd + 1, mine->send ? 0 : &set, mine->send ? &set : 0, 0, &t);
            if (ov->internal != STATUS_PENDING)
                break;                          /* cancelled meanwhile */
        }
        mine->used = 0;
    }
    if (transferred)
        *transferred = ov->internal_high;
    if (flags)
        *flags = 0;
    if (ov->internal) {
        ws_set_error((s32)ov->internal);
        return 0;
    }
    return 1;                                   /* TRUE */
}

/* NetDll_WSACancelOverlappedIO (17): every job of the socket ends, aborted. */
s32 ws_WSACancelOverlappedIO(u32 caller, u32 s)
{
    u32 i;

    (void)caller;
    for (i = 0; i < JOBS; i++)
        if (job[i].used == 2 && job[i].handle == s) {
            finish(&job[i], WSA_OPERATION_ABORTED, 0);
            job[i].used = 0;
        }
    return 0;
}

/* NetDll_WSAEventSelect (35) */
s32 ws_WSAEventSelect(u32 caller, u32 s, void *event, u32 events)
{
    s32 fd = ws_fd(s);
    u32 i, free_at = WATCHED, irql;
    int one = 1;

    (void)caller;
    if (fd < 0)
        return WS_ERROR;
    ws_stats.calls++;
    irql = KfAcquireSpinLock(&lock);
    for (i = 0; i < WATCHED; i++) {
        if (watch[i].handle == s)
            break;
        if (!watch[i].handle && free_at == WATCHED)
            free_at = i;
    }
    if (i == WATCHED)
        i = free_at;
    if (i == WATCHED) {
        KfReleaseSpinLock(&lock, irql);
        return ws_fail(WSAENOBUFS);
    }
    if (!events || !event) {
        watch[i].handle = 0;
    } else {
        watch[i].handle = s;
        watch[i].fd = fd;
        watch[i].event = event;
        watch[i].events = events;
        watch[i].write_armed = 1;
    }
    KfReleaseSpinLock(&lock, irql);
    if (events)
        lwip_ioctl(fd, FIONBIO, &one);          /* as Winsock: the socket no longer waits */
    start_watcher();
    return 0;
}

/* calls.c: a send on a watched socket would have waited -- FD_WRITE is due again. */
void ws_write_blocked(u32 s)
{
    u32 i;

    for (i = 0; i < WATCHED; i++)
        if (watch[i].handle == s)
            watch[i].write_armed = 1;
}

/* A socket closed: its jobs end and it is no longer watched. */
void ws_overlap_forget(u32 s)
{
    u32 i;

    for (i = 0; i < WATCHED; i++)
        if (watch[i].handle == s)
            watch[i].handle = 0;
    ws_WSACancelOverlappedIO(0, s);
}

static void watcher(void *arg)
{
    (void)arg;
    for (;;) {
        fd_set r, w;
        struct timeval t;
        int top = 0, n;
        u32 i;

        FD_ZERO(&r);
        FD_ZERO(&w);
        for (i = 0; i < JOBS; i++)
            if (job[i].used == 2) {
                if (job[i].send)
                    FD_SET(job[i].fd, &w);
                else
                    FD_SET(job[i].fd, &r);
                if (job[i].fd + 1 > top)
                    top = job[i].fd + 1;
            }
        for (i = 0; i < WATCHED; i++)
            if (watch[i].handle) {
                if (watch[i].events & (FD_READ | FD_ACCEPT | FD_CLOSE | FD_OOB))
                    FD_SET(watch[i].fd, &r);
                if (watch[i].write_armed && (watch[i].events & (FD_WRITE | FD_CONNECT)))
                    FD_SET(watch[i].fd, &w);
                if (watch[i].fd + 1 > top)
                    top = watch[i].fd + 1;
            }
        if (!top) {
            sys_msleep(10);
            continue;
        }
        t.tv_sec = 0;
        t.tv_usec = 2000;                       /* a job added meanwhile is seen this late */
        n = lwip_select(top, &r, &w, 0, &t);
        if (n <= 0)
            continue;
        for (i = 0; i < JOBS; i++) {
            JOB *j = &job[i];
            int got;

            if (j->used != 2 || !FD_ISSET(j->fd, j->send ? &w : &r))
                continue;
            got = attempt(j);
            if (got >= 0)
                finish(j, 0, (u32)got);
            else if (!would_wait())
                finish(j, WSAECONNRESET, 0);
            else
                continue;
            j->used = 0;
        }
        n = 0;
        for (i = 0; i < WATCHED; i++) {
            if (!watch[i].handle)
                continue;
            if (FD_ISSET(watch[i].fd, &r) ||
                (watch[i].write_armed && FD_ISSET(watch[i].fd, &w))) {
                if (FD_ISSET(watch[i].fd, &w))
                    watch[i].write_armed = 0;
                NtSetEvent(watch[i].event, 0);
                n = 1;
            }
        }
        if (n)
            sys_msleep(1);                      /* a level that stays: not a spin */
    }
}

#endif /* XE_STACK */
