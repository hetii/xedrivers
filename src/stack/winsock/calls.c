/* The socket calls. Each takes xnet's arguments (the caller first: XNCALLER_TITLE or
 * _SYSAPP), turns handles into lwIP descriptors and Winsock's layouts into lwIP's, calls
 * lwIP, and fails as Winsock does: WS_ERROR (or INVALID_SOCKET) and the code in
 * WSAGetLastError. A call made before the stack is up waits for it (ws_ready).
 */
#include "lwip/opt.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"

#include "winsock.h"
#include "ipstack.h"

#if XE_STACK

volatile WS_STATS ws_stats;

#define READY_WAIT_MS   30000
#define IOVECS          16

u32 ws_ready(void)
{
    u32 waited = 0;

    if (ipstack_ready)
        return 0;
    ws_stats.waited++;
    while (!ipstack_ready && waited < READY_WAIT_MS) {
        sys_msleep(10);
        waited += 10;
    }
    if (ipstack_ready)
        return 0;
    ws_set_error(WSAENETDOWN);
    return 1;
}

static s32 done(int r)
{
    return r < 0 ? ws_fail_errno() : r;
}

static int flags_in(u32 flags)
{
    return (flags & WS_MSG_PEEK ? MSG_PEEK : 0) | (flags & WS_MSG_OOB ? MSG_OOB : 0) |
           (flags & WS_MSG_WAITALL ? MSG_WAITALL : 0);
}

static s32 unsupported(void)
{
    ws_stats.unsupported++;
    return ws_fail(WSAEOPNOTSUPP);
}

/* NetDll_socket (3) */
u32 ws_socket(u32 caller, s32 family, s32 type, s32 protocol)
{
    int fd;

    if (ws_ready())
        return WS_INVALID_SOCKET;
    ws_stats.calls++;
    if (family != WS_AF_INET) {
        ws_set_error(WSAEAFNOSUPPORT);
        return WS_INVALID_SOCKET;
    }
    if ((type == WS_SOCK_STREAM && protocol && protocol != WS_IPPROTO_TCP) ||
        (type == WS_SOCK_DGRAM && protocol && protocol != WS_IPPROTO_UDP) ||
        (type != WS_SOCK_STREAM && type != WS_SOCK_DGRAM)) {
        ws_set_error(WSAEPROTONOSUPPORT);       /* VDP, a game's: xnet's secure transport */
        return WS_INVALID_SOCKET;
    }
    fd = lwip_socket(AF_INET, type == WS_SOCK_STREAM ? SOCK_STREAM : SOCK_DGRAM, 0);
    if (fd < 0) {
        ws_fail_errno();
        return WS_INVALID_SOCKET;
    }
    return ws_handle(fd, caller);
}

/* NetDll_closesocket (4) */
s32 ws_closesocket(u32 caller, u32 s)
{
    s32 fd = ws_fd(s);

    (void)caller;
    if (fd < 0)
        return WS_ERROR;
    ws_stats.calls++;
    ws_forget(fd);
    return lwip_close(fd) < 0 ? ws_fail_errno() : 0;
}

/* NetDll_shutdown (5): SD_RECEIVE, SD_SEND, SD_BOTH are SHUT_RD, SHUT_WR, SHUT_RDWR. */
s32 ws_shutdown(u32 caller, u32 s, s32 how)
{
    s32 fd = ws_fd(s);

    (void)caller;
    if (fd < 0)
        return WS_ERROR;
    ws_stats.calls++;
    return lwip_shutdown(fd, how) < 0 ? ws_fail_errno() : 0;
}

/* NetDll_ioctlsocket (6): FIONBIO and FIONREAD have BSD's numbers on both. */
s32 ws_ioctlsocket(u32 caller, u32 s, s32 command, u32 *argument)
{
    s32 fd = ws_fd(s);

    (void)caller;
    if (fd < 0)
        return WS_ERROR;
    ws_stats.calls++;
    return lwip_ioctl(fd, command, argument) < 0 ? ws_fail_errno() : 0;
}

/* NetDll_bind (11) */
s32 ws_bind(u32 caller, u32 s, const WS_SOCKADDR_IN *address, s32 length)
{
    struct sockaddr_in a;
    s32 fd = ws_fd(s);

    (void)caller;
    if (fd < 0)
        return WS_ERROR;
    ws_stats.calls++;
    if (!address || length < (s32)sizeof(WS_SOCKADDR_IN) - 8)
        return ws_fail(WSAEFAULT);
    ws_to_lwip(address, &a);
    {
        int type = 0;
        socklen_t n = sizeof(type);

        lwip_getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &n);
        ws_stats.bound[__sync_fetch_and_add(&ws_stats.binds, 1) & 7] =
            (u32)type << 16 | address->port;
    }
    return lwip_bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 ? ws_fail_errno() : 0;
}

/* NetDll_connect (12) */
s32 ws_connect(u32 caller, u32 s, const WS_SOCKADDR_IN *address, s32 length)
{
    struct sockaddr_in a;
    s32 fd = ws_fd(s);

    (void)caller;
    if (fd < 0)
        return WS_ERROR;
    ws_stats.calls++;
    if (!address || length < (s32)sizeof(WS_SOCKADDR_IN) - 8)
        return ws_fail(WSAEFAULT);
    ws_to_lwip(address, &a);
    if (lwip_connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        /* a non-blocking connect under way: Winsock says WSAEWOULDBLOCK, not EINPROGRESS */
        if (*ipstack_errno() == EINPROGRESS)
            return ws_fail(WSAEWOULDBLOCK);
        return ws_fail_errno();
    }
    return 0;
}

/* NetDll_listen (13) */
s32 ws_listen(u32 caller, u32 s, s32 backlog)
{
    s32 fd = ws_fd(s);

    (void)caller;
    if (fd < 0)
        return WS_ERROR;
    ws_stats.calls++;
    return lwip_listen(fd, backlog) < 0 ? ws_fail_errno() : 0;
}

/* NetDll_accept (14) */
u32 ws_accept(u32 caller, u32 s, WS_SOCKADDR_IN *address, s32 *length)
{
    struct sockaddr_in a;
    socklen_t n = sizeof(a);
    s32 fd = ws_fd(s);
    int c;

    if (fd < 0)
        return WS_INVALID_SOCKET;
    ws_stats.calls++;
    c = lwip_accept(fd, (struct sockaddr *)&a, &n);
    if (c < 0) {
        ws_fail_errno();
        return WS_INVALID_SOCKET;
    }
    if (address && length && *length >= (s32)sizeof(WS_SOCKADDR_IN)) {
        ws_from_lwip(&a, address);
        *length = sizeof(WS_SOCKADDR_IN);
    }
    return ws_handle(c, caller);
}

static s32 name(u32 s, WS_SOCKADDR_IN *address, s32 *length, u32 peer)
{
    struct sockaddr_in a;
    socklen_t n = sizeof(a);
    s32 fd = ws_fd(s);

    if (fd < 0)
        return WS_ERROR;
    ws_stats.calls++;
    if (!address || !length || *length < (s32)sizeof(WS_SOCKADDR_IN))
        return ws_fail(WSAEFAULT);
    if ((peer ? lwip_getpeername(fd, (struct sockaddr *)&a, &n)
              : lwip_getsockname(fd, (struct sockaddr *)&a, &n)) < 0)
        return ws_fail_errno();
    ws_from_lwip(&a, address);
    *length = sizeof(WS_SOCKADDR_IN);
    return 0;
}

/* NetDll_getsockname (9), NetDll_getpeername (10) */
s32 ws_getsockname(u32 caller, u32 s, WS_SOCKADDR_IN *address, s32 *length)
{
    (void)caller;
    return name(s, address, length, 0);
}

s32 ws_getpeername(u32 caller, u32 s, WS_SOCKADDR_IN *address, s32 *length)
{
    (void)caller;
    return name(s, address, length, 1);
}

/* NetDll_recv (18) */
s32 ws_recv(u32 caller, u32 s, void *buffer, s32 length, u32 flags)
{
    s32 fd = ws_fd(s);

    (void)caller;
    if (fd < 0)
        return WS_ERROR;
    ws_stats.calls++;
    return done((int)lwip_recv(fd, buffer, (size_t)length, flags_in(flags)));
}

/* NetDll_recvfrom (20) */
s32 ws_recvfrom(u32 caller, u32 s, void *buffer, s32 length, u32 flags,
                WS_SOCKADDR_IN *from, s32 *fromlength)
{
    struct sockaddr_in a;
    socklen_t n = sizeof(a);
    s32 fd = ws_fd(s);
    int r;

    (void)caller;
    if (fd < 0)
        return WS_ERROR;
    ws_stats.calls++;
    r = (int)lwip_recvfrom(fd, buffer, (size_t)length, flags_in(flags), (struct sockaddr *)&a,
                           &n);
    if (r < 0)
        return ws_fail_errno();
    if (from && fromlength && *fromlength >= (s32)sizeof(WS_SOCKADDR_IN)) {
        ws_from_lwip(&a, from);
        *fromlength = sizeof(WS_SOCKADDR_IN);
    }
    return r;
}

/* NetDll_send (22) */
s32 ws_send(u32 caller, u32 s, const void *buffer, s32 length, u32 flags)
{
    s32 fd = ws_fd(s);

    (void)caller;
    if (fd < 0)
        return WS_ERROR;
    ws_stats.calls++;
    return done((int)lwip_send(fd, buffer, (size_t)length, flags_in(flags)));
}

/* NetDll_sendto (24) */
s32 ws_sendto(u32 caller, u32 s, const void *buffer, s32 length, u32 flags,
              const WS_SOCKADDR_IN *to, s32 tolength)
{
    struct sockaddr_in a;
    s32 fd = ws_fd(s);

    (void)caller;
    if (fd < 0)
        return WS_ERROR;
    ws_stats.calls++;
    if (!to)
        return done((int)lwip_send(fd, buffer, (size_t)length, flags_in(flags)));
    if (tolength < (s32)sizeof(WS_SOCKADDR_IN) - 8)
        return ws_fail(WSAEFAULT);
    ws_to_lwip(to, &a);
    return done((int)lwip_sendto(fd, buffer, (size_t)length, flags_in(flags),
                                 (struct sockaddr *)&a, sizeof(a)));
}

/* NetDll_WSARecv (19): eight arguments, the last two the OVERLAPPED and its routine. */
s32 ws_WSARecv(u32 caller, u32 s, WS_BUF *buffers, u32 count, u32 *received, u32 *flags,
               void *overlapped, void *completion)
{
    struct iovec v[IOVECS];
    s32 fd = ws_fd(s);
    int r;
    u32 i;

    (void)caller; (void)completion;
    if (fd < 0)
        return WS_ERROR;
    if (overlapped)
        return unsupported();
    ws_stats.calls++;
    if (!buffers || !count || count > IOVECS)
        return ws_fail(WSAEINVAL);
    for (i = 0; i < count; i++) {
        v[i].iov_base = buffers[i].buffer;
        v[i].iov_len = buffers[i].length;
    }
    if (count == 1)
        r = (int)lwip_recv(fd, v[0].iov_base, v[0].iov_len, flags ? flags_in(*flags) : 0);
    else
        r = (int)lwip_readv(fd, v, (int)count);
    if (r < 0)
        return ws_fail_errno();
    if (received)
        *received = (u32)r;
    if (flags)
        *flags = 0;
    return 0;
}

/* NetDll_WSASend (23) */
s32 ws_WSASend(u32 caller, u32 s, const WS_BUF *buffers, u32 count, u32 *sent, u32 flags,
               void *overlapped, void *completion)
{
    struct iovec v[IOVECS];
    s32 fd = ws_fd(s);
    int r;
    u32 i;

    (void)caller; (void)completion;
    if (fd < 0)
        return WS_ERROR;
    if (overlapped)
        return unsupported();
    ws_stats.calls++;
    if (!buffers || !count || count > IOVECS)
        return ws_fail(WSAEINVAL);
    for (i = 0; i < count; i++) {
        v[i].iov_base = buffers[i].buffer;
        v[i].iov_len = buffers[i].length;
    }
    r = count == 1 ? (int)lwip_send(fd, v[0].iov_base, v[0].iov_len, flags_in(flags))
                   : (int)lwip_writev(fd, v, (int)count);
    if (r < 0)
        return ws_fail_errno();
    if (sent)
        *sent = (u32)r;
    return 0;
}

/* Not served yet (winsock.h): NetDll_WSARecvFrom (21) and NetDll_WSASendTo (25) take ten
 * arguments, NetDll_WSAGetOverlappedResult (16), NetDll_WSACancelOverlappedIO (17),
 * NetDll_WSAEventSelect (35). */
s32 ws_unsupported(void)
{
    return unsupported();
}

#endif /* XE_STACK */
