/* srv.h's sockets on the console: xam's NetDll_* by ordinal, as the bench's TCP
 * server calls them (caller kind 2, a system app). Every socket gets xnet's two
 * insecure-socket options or xnet drops its SYNs (bench/tcp/tcp.c). Addresses and
 * ports in a sockaddr are already in network order on this big-endian console.
 */

#include "srv.h"

extern int NetDll_WSAStartup(u32 caller, u16 version, void *data);
extern u32 NetDll_socket(u32 caller, int family, int type, int protocol);
extern int NetDll_closesocket(u32 caller, u32 socket);
extern int NetDll_setsockopt(u32 caller, u32 socket, int level, int option,
                             const void *value, int length);
extern int NetDll_getsockname(u32 caller, u32 socket, void *address, int *length);
extern int NetDll_bind(u32 caller, u32 socket, const void *address, int length);
extern int NetDll_connect(u32 caller, u32 socket, const void *address, int length);
extern int NetDll_listen(u32 caller, u32 socket, int backlog);
extern u32 NetDll_accept(u32 caller, u32 socket, void *address, int *length);
extern int NetDll_recv(u32 caller, u32 socket, void *buffer, int length, int flags);
extern int NetDll_send(u32 caller, u32 socket, const void *buffer, int length, int flags);
extern int NetDll_recvfrom(u32 caller, u32 socket, void *buffer, int length, int flags,
                           void *from, int *from_length);
extern int NetDll_sendto(u32 caller, u32 socket, const void *buffer, int length, int flags,
                         const void *to, int to_length);
extern int NetDll_XNetGetTitleXnAddr(u32 caller, void *xnaddr);
extern int NetDll_WSAGetLastError(void);
extern int NetDll_select(u32 caller, int nfds, void *readfds, void *writefds, void *exceptfds,
                         const void *timeout);

#define CALLER_SYSAPP   2
#define AF_INET         2
#define SOCK_STREAM     1
#define SOCK_DGRAM      2
#define IPPROTO_TCP     6
#define IPPROTO_UDP     17
#define SOL_SOCKET      0xFFFF
#define SO_REUSEADDR    0x0004
#define SO_SNDTIMEO     0x1005
#define SO_RCVBUF       0x1002
#define SEND_TIMEOUT_MS 60000u              /* a send that cannot move this long fails */
#define SO_XNET_5802    0x5802
#define SO_XNET_5801    0x5801
#define INVALID_SOCKET  0xFFFFFFFFu

typedef struct {
    u16 family;
    u16 port;
    u32 address;
    u8  zero[8];
} SOCKADDR_IN;

static volatile u32 started;

static void start_once(void)
{
    static u32 wsa[128];

    if (!started) {
        NetDll_WSAStartup(CALLER_SYSAPP, 0x0202, wsa);
        started = 1;
    }
}

/* --- sockets ----------------------------------------------------------------- */

static u32 insecure(u32 s)
{
    u32 one = 1;

    return NetDll_setsockopt(CALLER_SYSAPP, s, SOL_SOCKET, SO_XNET_5802, &one, 4) >= 0 &&
           NetDll_setsockopt(CALLER_SYSAPP, s, SOL_SOCKET, SO_XNET_5801, &one, 4) >= 0;
}

static SRV_SOCK bound(int type, int protocol, u32 port)
{
    SOCKADDR_IN a;
    u32 s, i, one = 1;

    start_once();
    s = NetDll_socket(CALLER_SYSAPP, AF_INET, type, protocol);
    if (s == INVALID_SOCKET)
        return -1;
    if (!insecure(s)) {
        NetDll_closesocket(CALLER_SYSAPP, s);
        return -1;
    }
    NetDll_setsockopt(CALLER_SYSAPP, s, SOL_SOCKET, SO_REUSEADDR, &one, 4);
    a.family = AF_INET;
    a.port = (u16)port;
    a.address = 0;
    for (i = 0; i < sizeof(a.zero); i++)
        a.zero[i] = 0;
    if (NetDll_bind(CALLER_SYSAPP, s, &a, sizeof(a)) < 0) {
        NetDll_closesocket(CALLER_SYSAPP, s);
        return -1;
    }
    return (SRV_SOCK)s;
}

SRV_SOCK srv_listen(u32 port, u32 backlog)
{
    SRV_SOCK s = bound(SOCK_STREAM, IPPROTO_TCP, port);

    if (s >= 0 && NetDll_listen(CALLER_SYSAPP, (u32)s, (int)backlog) < 0) {
        NetDll_closesocket(CALLER_SYSAPP, (u32)s);
        return -1;
    }
    return s;
}

SRV_SOCK srv_udp(u32 port)
{
    return bound(SOCK_DGRAM, IPPROTO_UDP, port);
}

SRV_SOCK srv_accept(SRV_SOCK listener, u32 *peer)
{
    SOCKADDR_IN a;
    int n = sizeof(a);
    u32 s = NetDll_accept(CALLER_SYSAPP, (u32)listener, &a, &n);

    if (s == INVALID_SOCKET)
        return -1;
    insecure(s);
    {
        u32 ms = SEND_TIMEOUT_MS;

        NetDll_setsockopt(CALLER_SYSAPP, s, SOL_SOCKET, SO_SNDTIMEO, &ms, 4);
    }
    if (peer)
        *peer = a.address;
    return (SRV_SOCK)s;
}

SRV_SOCK srv_connect(u32 address, u32 port)
{
    SOCKADDR_IN a;
    u32 s, i;

    start_once();
    s = NetDll_socket(CALLER_SYSAPP, AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
        return -1;
    insecure(s);
    a.family = AF_INET;
    a.port = (u16)port;
    a.address = address;
    for (i = 0; i < sizeof(a.zero); i++)
        a.zero[i] = 0;
    if (NetDll_connect(CALLER_SYSAPP, s, &a, sizeof(a)) < 0) {
        NetDll_closesocket(CALLER_SYSAPP, s);
        return -1;
    }
    return (SRV_SOCK)s;
}

s32 srv_recv(SRV_SOCK s, void *buffer, u32 length)
{
    int n = NetDll_recv(CALLER_SYSAPP, (u32)s, buffer, (int)length, 0);

    return n < 0 ? -1 : n;
}

/* What sending met, read by tools on the host (xectl): xnet runs out of send
 * buffers with eight connections sending at once (2026-11-05), and a send that
 * failed with WSAENOBUFS or WSAEWOULDBLOCK is tried again after 5 ms, for 10 s. */
volatile u32 srv_send_retries, srv_send_last_error;

#define WSAEWOULDBLOCK  10035
#define WSAENOBUFS      10055
#define SEND_RETRIES    2000u

s32 srv_send(SRV_SOCK s, const void *buffer, u32 length)
{
    const u8 *p = (const u8 *)buffer;
    u32 done = 0, tries = 0;
    int n, e;

    while (done < length) {
        n = NetDll_send(CALLER_SYSAPP, (u32)s, p + done, (int)(length - done), 0);
        if (n > 0) {
            done += (u32)n;
            continue;
        }
        e = NetDll_WSAGetLastError();
        srv_send_last_error = (u32)e;
        if (n < 0 && (e == WSAENOBUFS || e == WSAEWOULDBLOCK) && tries++ < SEND_RETRIES) {
            srv_send_retries++;
            srv_sleep(5);
            continue;
        }
        return -1;
    }
    return (s32)done;
}

s32 srv_recvfrom(SRV_SOCK s, void *buffer, u32 length, u32 *address, u32 *port)
{
    SOCKADDR_IN a;
    int n = sizeof(a), r = NetDll_recvfrom(CALLER_SYSAPP, (u32)s, buffer, (int)length, 0, &a, &n);

    if (r < 0)
        return -1;
    *address = a.address;
    *port = a.port;
    return r;
}

s32 srv_sendto(SRV_SOCK s, const void *buffer, u32 length, u32 address, u32 port)
{
    SOCKADDR_IN a;
    u32 i;

    a.family = AF_INET;
    a.port = (u16)port;
    a.address = address;
    for (i = 0; i < sizeof(a.zero); i++)
        a.zero[i] = 0;
    return NetDll_sendto(CALLER_SYSAPP, (u32)s, buffer, (int)length, 0, &a, sizeof(a)) < 0
               ? -1 : (s32)length;
}

/* Winsock's fd_set and timeval, as xnet takes them. */
s32 srv_wait(SRV_SOCK s, u32 ms)
{
    struct {
        u32 count;
        u32 sockets[1];
    } set;
    struct {
        s32 sec, usec;
    } t;

    set.count = 1;
    set.sockets[0] = (u32)s;
    t.sec = (s32)(ms / 1000);
    t.usec = (s32)(ms % 1000 * 1000);
    return NetDll_select(CALLER_SYSAPP, 0, &set, 0, 0, &t);
}

void srv_receive_window(SRV_SOCK s, u32 bytes)
{
    NetDll_setsockopt(CALLER_SYSAPP, (u32)s, SOL_SOCKET, SO_RCVBUF, &bytes, 4);
}

void srv_close(SRV_SOCK s)
{
    if (s >= 0)
        NetDll_closesocket(CALLER_SYSAPP, (u32)s);
}

/* xnet's getsockname leaves a connection accepted on a listener bound to every
 * address at 0.0.0.0 (rpcbind answered "0.0.0.0.0.111", 2026-11-04): then the
 * console's one address. */
u32 srv_local_address(SRV_SOCK s)
{
    SOCKADDR_IN a;
    int n = sizeof(a);

    a.address = 0;
    if (NetDll_getsockname(CALLER_SYSAPP, (u32)s, &a, &n) < 0 || !a.address)
        return srv_host_address(0);
    return a.address;
}

u32 srv_local_port(SRV_SOCK s)
{
    SOCKADDR_IN a;
    int n = sizeof(a);

    return NetDll_getsockname(CALLER_SYSAPP, (u32)s, &a, &n) < 0 ? 0 : a.port;
}

/* XNADDR: the title's address first; one interface, so it is the address for
 * every peer. */
u32 srv_host_address(u32 peer)
{
    struct {
        u32 ina, ina_online;
        u16 port_online;
        u8 enet[6];
        u8 online[20];
    } x;

    (void)peer;
    start_once();
    x.ina = 0;
    NetDll_XNetGetTitleXnAddr(CALLER_SYSAPP, &x);
    return x.ina;
}
