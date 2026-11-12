/* A socket's handle. Winsock hands out opaque SOCKETs; the handle here is lwIP's
 * descriptor plus HANDLE_BASE, so it is never 0 and never INVALID_SOCKET, and a handle
 * xnet gave out before the hooks were in place (it is no socket of ours) is told apart.
 * The caller that made each socket is kept: WSACleanup closes that caller's.
 */
#include "lwip/opt.h"
#include "lwip/sockets.h"

#include "winsock.h"

#if XE_STACK

#define HANDLE_BASE 0x4000u
#define FDS         (MEMP_NUM_NETCONN + LWIP_SOCKET_OFFSET)

static u8 used[FDS];
static u32 caller_of[FDS];

s32 ws_fd(u32 handle)
{
    u32 fd = handle - HANDLE_BASE;

    if (handle < HANDLE_BASE || fd >= FDS || !used[fd]) {
        ws_stats.not_ours++;
        ws_stats.not_ours_from = (u32)__builtin_return_address(0);
        ws_stats.not_ours_handle = handle;
        ws_set_error(WSAENOTSOCK);
        return -1;
    }
    {
        u32 from = (u32)__builtin_return_address(0), i;

        for (i = 0; i < 16; i++) {
            if (ws_stats.by_caller[i][0] == from || !ws_stats.by_caller[i][0]) {
                ws_stats.by_caller[i][0] = from;
                ws_stats.by_caller[i][1]++;
                break;
            }
        }
    }
    return (s32)fd;
}

u32 ws_handle(s32 fd, u32 caller)
{
    if (fd < 0 || (u32)fd >= FDS)
        return WS_INVALID_SOCKET;
    caller_of[fd] = caller;
    used[fd] = 1;
    if (__sync_add_and_fetch(&ws_stats.sockets, 1) > ws_stats.sockets_most)
        ws_stats.sockets_most = ws_stats.sockets;
    return HANDLE_BASE + (u32)fd;
}

void ws_forget(s32 fd)
{
    if (fd >= 0 && (u32)fd < FDS && used[fd]) {
        used[fd] = 0;
        __sync_sub_and_fetch(&ws_stats.sockets, 1);
    }
}

void ws_close_caller(u32 caller)
{
    u32 fd;

    for (fd = 0; fd < FDS; fd++)
        if (used[fd] && caller_of[fd] == caller) {
            ws_forget((s32)fd);
            lwip_close((int)fd);
        }
}

#endif /* XE_STACK */
