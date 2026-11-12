/* setsockopt / getsockopt. Winsock's SOL_SOCKET is 0xFFFF, lwIP's 0xFFF; most option
 * numbers are the same (BSD's). What differs: the time-outs are milliseconds in a DWORD,
 * not a timeval; struct linger is two 16-bit words; IP_TOS and IP_TTL are numbered
 * otherwise. Buffer sizes are taken and not applied: lwIP's window and buffers are its own
 * and larger than what programs ask (that is the point of this stack). xnet's own options
 * (0x5800-0x58FF, the "insecure" switches every system program sets) are taken as done.
 */
#include "lwip/opt.h"
#include "lwip/sockets.h"
#include "lwip/tcp.h"

#include "winsock.h"

#if XE_STACK

#define WS_SO_DEBUG         0x0001
#define WS_SO_ACCEPTCONN    0x0002
#define WS_SO_REUSEADDR     0x0004
#define WS_SO_KEEPALIVE     0x0008
#define WS_SO_BROADCAST     0x0020
#define WS_SO_LINGER        0x0080
#define WS_SO_DONTLINGER    0xFF7F
#define WS_SO_SNDBUF        0x1001
#define WS_SO_RCVBUF        0x1002
#define WS_SO_SNDTIMEO      0x1005
#define WS_SO_RCVTIMEO      0x1006
#define WS_SO_ERROR         0x1007
#define WS_SO_TYPE          0x1008
#define WS_IP_TOS           3
#define WS_IP_TTL           4
#define WS_TCP_NODELAY      1

static u32 word(const void *value, s32 length)
{
    if (!value || length < 1)
        return 0;
    if (length >= 4)
        return *(const u32 *)value;
    if (length >= 2)
        return *(const u16 *)value;
    return *(const u8 *)value;
}

static u32 asked;                       /* level << 16 | option of the call under way */

static s32 done(int r)
{
    if (r < 0)
        ws_stats.option_failed = asked;
    return r < 0 ? ws_fail_errno() : 0;
}

/* NetDll_setsockopt (7) */
s32 ws_setsockopt(u32 caller, u32 s, s32 level, s32 option, const void *value, s32 length)
{
    s32 fd = ws_fd(s);
    int v;

    (void)caller;
    if (fd < 0)
        return WS_ERROR;
    ws_stats.calls++;
    asked = (u32)level << 16 | ((u32)option & 0xFFFFu);
    if (level == WS_SOL_SOCKET) {
        switch (option) {
        case WS_SO_REUSEADDR:
        case WS_SO_KEEPALIVE:
        case WS_SO_BROADCAST:
            v = word(value, length) != 0;
            return done(lwip_setsockopt(fd, SOL_SOCKET, option, &v, sizeof(v)));
        case WS_SO_SNDTIMEO:
        case WS_SO_RCVTIMEO: {
            u32 ms = word(value, length);
            struct timeval t;

            t.tv_sec = (long)(ms / 1000);
            t.tv_usec = (long)(ms % 1000 * 1000);
            return done(lwip_setsockopt(fd, SOL_SOCKET, option == WS_SO_SNDTIMEO ? SO_SNDTIMEO
                                        : SO_RCVTIMEO, &t, sizeof(t)));
        }
        case WS_SO_LINGER: {
            const u16 *w = (const u16 *)value;
            struct linger l;

            if (!value || length < 4)
                return ws_fail(WSAEFAULT);
            l.l_onoff = w[0] != 0;
            l.l_linger = w[1];
            return done(lwip_setsockopt(fd, SOL_SOCKET, SO_LINGER, &l, sizeof(l)));
        }
        case WS_SO_DONTLINGER: {
            struct linger l;

            l.l_onoff = word(value, length) == 0;
            l.l_linger = 0;
            return done(lwip_setsockopt(fd, SOL_SOCKET, SO_LINGER, &l, sizeof(l)));
        }
        case WS_SO_SNDBUF:
        case WS_SO_RCVBUF:
        case WS_SO_DEBUG:
            return 0;
        default:
            if (option >= 0x5800 && option <= 0x58FF)
                return 0;                       /* xnet's own */
            ws_stats.option_refused = (u32)level << 16 | ((u32)option & 0xFFFFu);
            return ws_fail(WSAENOPROTOOPT);
        }
    }
    if (level == WS_IPPROTO_TCP) {
        if (option != WS_TCP_NODELAY)
            return 0;                           /* xnet's TCP options: nothing to do here */
        v = word(value, length) != 0;
        return done(lwip_setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &v, sizeof(v)));
    }
    if (level == WS_IPPROTO_IP) {
        v = (int)word(value, length);
        if (option == WS_IP_TOS)
            return done(lwip_setsockopt(fd, IPPROTO_IP, IP_TOS, &v, sizeof(v)));
        if (option == WS_IP_TTL)
            return done(lwip_setsockopt(fd, IPPROTO_IP, IP_TTL, &v, sizeof(v)));
        return 0;
    }
    ws_stats.option_refused = (u32)level << 16 | ((u32)option & 0xFFFFu);
    return ws_fail(WSAEINVAL);
}

static s32 put(void *value, s32 *length, u32 v)
{
    if (!value || !length || *length < 4)
        return ws_fail(WSAEFAULT);
    *(u32 *)value = v;
    *length = 4;
    return 0;
}

/* NetDll_getsockopt (8) */
s32 ws_getsockopt(u32 caller, u32 s, s32 level, s32 option, void *value, s32 *length)
{
    s32 fd = ws_fd(s);
    int v = 0;
    socklen_t n = sizeof(v);

    (void)caller;
    if (fd < 0)
        return WS_ERROR;
    ws_stats.calls++;
    if (level == WS_SOL_SOCKET) {
        switch (option) {
        case WS_SO_REUSEADDR:
        case WS_SO_KEEPALIVE:
        case WS_SO_BROADCAST:
        case WS_SO_ERROR:
        case WS_SO_TYPE:
        case WS_SO_ACCEPTCONN:
            if (lwip_getsockopt(fd, SOL_SOCKET, option, &v, &n) < 0) {
                ws_stats.option_failed = 0x80000000u | (u32)option;
                return ws_fail_errno();
            }
            if (option == WS_SO_ERROR && v) {
                *ipstack_errno() = v;
                ws_fail_errno();                /* the code, as Winsock numbers it */
                v = ws_WSAGetLastError();
                ws_set_error(0);
            }
            return put(value, length, (u32)v);
        case WS_SO_SNDTIMEO:
        case WS_SO_RCVTIMEO: {
            struct timeval t;

            n = sizeof(t);
            if (lwip_getsockopt(fd, SOL_SOCKET, option == WS_SO_SNDTIMEO ? SO_SNDTIMEO
                                : SO_RCVTIMEO, &t, &n) < 0)
                return ws_fail_errno();
            return put(value, length, (u32)(t.tv_sec * 1000 + t.tv_usec / 1000));
        }
        case WS_SO_SNDBUF:
            return put(value, length, TCP_SND_BUF);
        case WS_SO_RCVBUF:
            return put(value, length, TCP_WND);
        default:
            if (option >= 0x5800 && option <= 0x58FF)
                return put(value, length, 1);
            return ws_fail(WSAENOPROTOOPT);
        }
    }
    if (level == WS_IPPROTO_TCP && option == WS_TCP_NODELAY) {
        if (lwip_getsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &v, &n) < 0)
            return ws_fail_errno();
        return put(value, length, (u32)v);
    }
    return ws_fail(WSAENOPROTOOPT);
}

#endif /* XE_STACK */
