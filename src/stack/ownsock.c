/* The file servers' sockets on the own stack: what src/servers/common/srv_net.c does on
 * xnet, done with lwIP's socket API. srv_net.c hands every call here when
 * `stack_servers = 1` is in xedrivers.ini (ipstack_servers), so the same xeftp and xenfs
 * run on either stack and the two can be measured against each other.
 *
 * Every local port a socket here is bound to is entered in the stack's port table
 * (ipstack_port): that is what makes its frames the stack's and not xnet's.
 */
#include "lwip/opt.h"
#include "lwip/sockets.h"
#include "lwip/netif.h"
#include <string.h>

#include "ipstack.h"

#if XE_STACK

#define SEND_TIMEOUT_MS 30000

static u16 bound_port[MEMP_NUM_NETCONN + 8];    /* by socket: the port it holds in the table */

static struct sockaddr_in at(u32 address, u32 port)
{
    struct sockaddr_in a;

    memset(&a, 0, sizeof(a));
    a.sin_len = sizeof(a);
    a.sin_family = AF_INET;
    a.sin_port = (u16_t)port;               /* big-endian machine: host order is the wire's */
    a.sin_addr.s_addr = address;
    return a;
}

static u32 port_of(int s)
{
    struct sockaddr_in a;
    socklen_t n = sizeof(a);

    return lwip_getsockname(s, (struct sockaddr *)&a, &n) < 0 ? 0 : a.sin_port;
}

/* A socket bound to `port` (0: one lwIP picks), its port in the table. */
static s32 bound(int type, u32 port)
{
    struct sockaddr_in a = at(0, port);
    int s = lwip_socket(AF_INET, type, 0), one = 1;
    u32 got;

    if (s < 0)
        return -1;
    lwip_setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (lwip_bind(s, (struct sockaddr *)&a, sizeof(a)) < 0 || !(got = port_of(s)) ||
        (u32)s >= sizeof(bound_port) / sizeof(bound_port[0])) {
        lwip_close(s);
        return -1;
    }
    bound_port[s] = (u16)got;
    ipstack_port(got, 1);
    return s;
}

s32 own_listen(u32 port, u32 backlog)
{
    s32 s = bound(SOCK_STREAM, port);

    if (s >= 0 && lwip_listen(s, (int)backlog) < 0) {
        own_close(s);
        return -1;
    }
    return s;
}

s32 own_udp(u32 port)
{
    return bound(SOCK_DGRAM, port);
}

s32 own_accept(s32 listener, u32 *peer)
{
    struct sockaddr_in a;
    socklen_t n = sizeof(a);
    struct timeval t;
    int s = lwip_accept(listener, (struct sockaddr *)&a, &n);

    if (s < 0)
        return -1;
    t.tv_sec = SEND_TIMEOUT_MS / 1000;
    t.tv_usec = 0;
    lwip_setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &t, sizeof(t));
    {
        /* a reply is written in pieces (an RPC header, then the data): held back for the
         * peer's delayed acknowledgement, the last piece of each waits 40 ms */
        int one = 1;

        lwip_setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }
    if (peer)
        *peer = a.sin_addr.s_addr;
    /* the connection holds its listener's port too: it may outlive the listener */
    if ((u32)s < sizeof(bound_port) / sizeof(bound_port[0]) && (bound_port[s] = (u16)port_of(s)) != 0)
        ipstack_port(bound_port[s], 1);
    return s;
}

s32 own_connect(u32 address, u32 port)
{
    struct sockaddr_in a = at(address, port);
    s32 s = bound(SOCK_STREAM, 0);

    if (s >= 0 && lwip_connect(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
        own_close(s);
        return -1;
    }
    return s;
}

s32 own_recv(s32 s, void *buffer, u32 length)
{
    int n = (int)lwip_recv(s, buffer, length, 0);

    return n < 0 ? -1 : n;
}

s32 own_send(s32 s, const void *buffer, u32 length)
{
    const u8 *p = (const u8 *)buffer;
    u32 done = 0;

    while (done < length) {
        int n = (int)lwip_send(s, p + done, length - done, 0);

        if (n <= 0)
            return -1;
        done += (u32)n;
    }
    return (s32)done;
}

s32 own_recvfrom(s32 s, void *buffer, u32 length, u32 *address, u32 *port)
{
    struct sockaddr_in a;
    socklen_t n = sizeof(a);
    int r = (int)lwip_recvfrom(s, buffer, length, 0, (struct sockaddr *)&a, &n);

    if (r < 0)
        return -1;
    *address = a.sin_addr.s_addr;
    *port = a.sin_port;
    return r;
}

s32 own_sendto(s32 s, const void *buffer, u32 length, u32 address, u32 port)
{
    struct sockaddr_in a = at(address, port);

    return lwip_sendto(s, buffer, length, 0, (struct sockaddr *)&a, sizeof(a)) < 0
               ? -1 : (s32)length;
}

s32 own_wait(s32 s, u32 ms)
{
    fd_set set;
    struct timeval t;

    FD_ZERO(&set);
    FD_SET(s, &set);
    t.tv_sec = (long)(ms / 1000);
    t.tv_usec = (long)(ms % 1000 * 1000);
    return lwip_select(s + 1, &set, 0, 0, &t);
}

void own_close(s32 s)
{
    if (s < 0)
        return;
    if ((u32)s < sizeof(bound_port) / sizeof(bound_port[0]) && bound_port[s]) {
        ipstack_port(bound_port[s], 0);
        bound_port[s] = 0;
    }
    lwip_close(s);
}

u32 own_local_port(s32 s)
{
    return port_of(s);
}

u32 own_host_address(void)
{
    return ipstack_stats.ip;
}

#endif /* XE_STACK */
