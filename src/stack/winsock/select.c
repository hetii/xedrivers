/* select(). Winsock passes a count and an array of handles in each set and ignores nfds;
 * lwIP takes bitmaps of descriptors. The sets are turned into bitmaps, lwIP waits, and
 * each set is written back with the handles that are ready, in their order. The count
 * returned is all of them, over the three sets, as both Winsock and lwIP count it.
 */
#include "lwip/opt.h"
#include "lwip/sockets.h"

#include "winsock.h"

#if XE_STACK

static int fill(const WS_FD_SET *in, fd_set *out, int *top)
{
    u32 i;

    FD_ZERO(out);
    if (!in)
        return 0;
    for (i = 0; i < in->count && i < WS_FD_SETSIZE; i++) {
        s32 fd = ws_fd(in->handle[i]);

        if (fd < 0)
            return -1;
        FD_SET(fd, out);
        if (fd + 1 > *top)
            *top = fd + 1;
    }
    return 0;
}

static void back(WS_FD_SET *set, const fd_set *ready)
{
    u32 i, n = 0;

    if (!set)
        return;
    for (i = 0; i < set->count && i < WS_FD_SETSIZE; i++) {
        s32 fd = ws_fd(set->handle[i]);

        if (fd >= 0 && FD_ISSET(fd, ready))
            set->handle[n++] = set->handle[i];
    }
    set->count = n;
}

/* NetDll_select (15) */
s32 ws_select(u32 caller, s32 nfds, WS_FD_SET *readfds, WS_FD_SET *writefds,
              WS_FD_SET *exceptfds, const WS_TIMEVAL *timeout)
{
    fd_set r, w, e;
    struct timeval t;
    int top = 0, n;

    (void)caller; (void)nfds;
    if (ws_ready())
        return WS_ERROR;
    ws_stats.calls++;
    if (fill(readfds, &r, &top) < 0 || fill(writefds, &w, &top) < 0 ||
        fill(exceptfds, &e, &top) < 0)
        return WS_ERROR;                        /* WSAENOTSOCK: one is no socket of ours */
    if (timeout) {
        t.tv_sec = timeout->sec;
        t.tv_usec = timeout->usec;
    }
    n = lwip_select(top, readfds ? &r : 0, writefds ? &w : 0, exceptfds ? &e : 0,
                    timeout ? &t : 0);
    if (n < 0)
        return ws_fail_errno();
    if (!n && timeout && !timeout->sec && !timeout->usec) {
        /* a poll that found nothing waits 1 ms. XBDM polls its UDP socket so in a loop of
         * its own, 680 000 times a second on lwIP (xnet's select waits on a kernel object
         * even then), each poll taking the lock the USB processor takes for every frame */
        s64 a_while = -10000;               /* 1 ms */

        KeDelayExecutionThread(0, 0, &a_while);
    }
    back(readfds, &r);
    back(writefds, &w);
    back(exceptfds, &e);
    if (readfds)
        ws_stats.select_ready[0] += readfds->count;
    if (writefds)
        ws_stats.select_ready[1] += writefds->count;
    if (exceptfds)
        ws_stats.select_ready[2] += exceptfds->count;
    if (!timeout)
        ws_stats.select_forever++;
    else if (!timeout->sec && !timeout->usec)
        ws_stats.select_poll++;
    else
        ws_stats.select_timed++;
    return n;
}

#endif /* XE_STACK */
