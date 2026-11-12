/* lwIP's errno and Winsock's last error, both per thread.
 *
 * Winsock's last error is the calling thread's own word, KTHREAD+0x160: xam 17559's
 * NetDll_WSAGetLastError and _WSASetLastError are two-word stubs that end up reading and
 * writing it (`lwz r11,0x100(r13); lwz r3,0x160(r11)`, and nothing at all when PCR+0x150
 * says a DPC runs). Too short to patch, and no need: a code written there is what xam's
 * own WSAGetLastError hands every caller.
 *
 * lwIP reports a failed call in errno, which one global would get wrong with many threads
 * at once: it lives in a small table by the calling thread. A thread that has ended leaves
 * its place behind; when the table is full a place is shared -- with 256 places and a
 * thread a connection that does not happen in practice.
 */
#include "lwip/opt.h"
#include "lwip/errno.h"

#include "winsock.h"

#if XE_STACK

#define PLACES  256u

static struct {
    void *volatile thread;
    int errno_value;
} place[PLACES];

static void *self(void)
{
    void *t;

    __asm__ volatile("lwz %0, 0x100(13)" : "=r"(t));
    return t;
}

static u32 slot(void)
{
    void *t = self();
    u32 h = ((u32)t >> 6) * 0x9E3779B1u >> 24, i;

    for (i = 0; i < PLACES; i++) {
        u32 at = (h + i) & (PLACES - 1);

        if (place[at].thread == t)
            return at;
        if (!place[at].thread &&
            __sync_bool_compare_and_swap(&place[at].thread, (void *)0, t))
            return at;
    }
    return h & (PLACES - 1);
}

int *ipstack_errno(void)
{
    return &place[slot()].errno_value;
}

static volatile s32 *thread_error(void)
{
    u8 *t = (u8 *)self();
    u32 dpc;

    __asm__ volatile("lwz %0, 0x150(13)" : "=r"(dpc));
    return dpc || !t ? 0 : (volatile s32 *)(t + 0x160);
}

void ws_set_error(s32 wsa)
{
    volatile s32 *e = thread_error();

    if (e)
        *e = wsa;
    if (wsa) {
        u32 at = __sync_fetch_and_add(&ws_stats.failed, 1) % WS_RECENT;

        ws_stats.recent[at][0] = (u32)__builtin_return_address(0);
        ws_stats.recent[at][1] = (u32)wsa;
        ws_stats.recent[at][2] = ws_stats.calls;
    }
}

s32 ws_fail(s32 wsa)
{
    ws_set_error(wsa);
    return WS_ERROR;
}

static s32 to_wsa(int e)
{
    switch (e) {
    case EINTR:         return WSAEINTR;
    case EBADF:         return WSAENOTSOCK;
    case EFAULT:        return WSAEFAULT;
    case EINVAL:        return WSAEINVAL;
    case EMFILE:
    case ENFILE:        return WSAEMFILE;
    case EWOULDBLOCK:   return WSAEWOULDBLOCK;
    case EINPROGRESS:   return WSAEINPROGRESS;
    case EALREADY:      return WSAEALREADY;
    case ENOTSOCK:      return WSAENOTSOCK;
    case EDESTADDRREQ:  return WSAEDESTADDRREQ;
    case EMSGSIZE:      return WSAEMSGSIZE;
    case EPROTOTYPE:    return WSAEPROTOTYPE;
    case ENOPROTOOPT:   return WSAENOPROTOOPT;
    case EPROTONOSUPPORT: return WSAEPROTONOSUPPORT;
    case EOPNOTSUPP:    return WSAEOPNOTSUPP;
    case EAFNOSUPPORT:  return WSAEAFNOSUPPORT;
    case EADDRINUSE:    return WSAEADDRINUSE;
    case EADDRNOTAVAIL: return WSAEADDRNOTAVAIL;
    case ENETDOWN:      return WSAENETDOWN;
    case ENETUNREACH:   return WSAENETUNREACH;
    case ECONNABORTED:  return WSAECONNABORTED;
    case ECONNRESET:    return WSAECONNRESET;
    case ENOMEM:
    case ENOBUFS:       return WSAENOBUFS;
    case EISCONN:       return WSAEISCONN;
    case ENOTCONN:      return WSAENOTCONN;
    case ESHUTDOWN:     return WSAESHUTDOWN;
    case ETIMEDOUT:     return WSAETIMEDOUT;
    case ECONNREFUSED:  return WSAECONNREFUSED;
    case EHOSTUNREACH:  return WSAEHOSTUNREACH;
    default:            return WSAEINVAL;
    }
}

s32 ws_fail_errno(void)
{
    ws_set_error(to_wsa(place[slot()].errno_value));
    return WS_ERROR;
}

/* What xam's NetDll_WSAGetLastError would answer this thread now. */
s32 ws_WSAGetLastError(void)
{
    volatile s32 *e = thread_error();

    return e ? *e : 0;
}

#endif /* XE_STACK */
