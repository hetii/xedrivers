/* srv.h on POSIX, for the host tests only: the servers of src/servers/ built and
 * run on the PC, mounted with the PC's own clients. Nothing here goes into the
 * plugin. Sockets. */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include "srv.h"

/* --- sockets ----------------------------------------------------------------- */

SRV_SOCK srv_listen(u32 port, u32 backlog)
{
    struct sockaddr_in a;
    int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;

    if (s < 0)
        return -1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((u16)port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(s, (int)backlog) < 0) {
        close(s);
        return -1;
    }
    return s;
}

SRV_SOCK srv_udp(u32 port)
{
    struct sockaddr_in a;
    int s = socket(AF_INET, SOCK_DGRAM, 0), one = 1;

    if (s < 0)
        return -1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((u16)port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
        close(s);
        return -1;
    }
    return s;
}

SRV_SOCK srv_accept(SRV_SOCK listener, u32 *peer)
{
    struct sockaddr_in a;
    socklen_t n = sizeof(a);
    int s = accept(listener, (struct sockaddr *)&a, &n), one = 1;

    if (s < 0)
        return -1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    if (peer)
        *peer = ntohl(a.sin_addr.s_addr);
    return s;
}

SRV_SOCK srv_connect(u32 address, u32 port)
{
    struct sockaddr_in a;
    int s = socket(AF_INET, SOCK_STREAM, 0);

    if (s < 0)
        return -1;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((u16)port);
    a.sin_addr.s_addr = htonl(address);
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
        close(s);
        return -1;
    }
    return s;
}

s32 srv_recv(SRV_SOCK s, void *buffer, u32 length)
{
    ssize_t n = recv(s, buffer, length, 0);

    return n < 0 ? -1 : (s32)n;
}

s32 srv_send(SRV_SOCK s, const void *buffer, u32 length)
{
    const char *p = buffer;
    u32 done = 0;

    while (done < length) {
        ssize_t n = send(s, p + done, length - done, MSG_NOSIGNAL);

        if (n <= 0)
            return -1;
        done += (u32)n;
    }
    return (s32)done;
}

s32 srv_recvfrom(SRV_SOCK s, void *buffer, u32 length, u32 *address, u32 *port)
{
    struct sockaddr_in a;
    socklen_t n = sizeof(a);
    ssize_t r = recvfrom(s, buffer, length, 0, (struct sockaddr *)&a, &n);

    if (r < 0)
        return -1;
    *address = ntohl(a.sin_addr.s_addr);
    *port = ntohs(a.sin_port);
    return (s32)r;
}

s32 srv_sendto(SRV_SOCK s, const void *buffer, u32 length, u32 address, u32 port)
{
    struct sockaddr_in a;

    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((u16)port);
    a.sin_addr.s_addr = htonl(address);
    return sendto(s, buffer, length, 0, (struct sockaddr *)&a, sizeof(a)) < 0 ? -1
                                                                             : (s32)length;
}

s32 srv_wait(SRV_SOCK s, u32 ms)
{
    struct pollfd p;

    p.fd = s;
    p.events = POLLIN;
    p.revents = 0;
    return poll(&p, 1, (int)ms);
}

void srv_buffers(SRV_SOCK s, u32 bytes)
{
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes));
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof(bytes));
}

void srv_close(SRV_SOCK s)
{
    if (s >= 0)
        close(s);
}

u32 srv_local_address(SRV_SOCK s)
{
    struct sockaddr_in a;
    socklen_t n = sizeof(a);

    return getsockname(s, (struct sockaddr *)&a, &n) < 0 ? 0 : ntohl(a.sin_addr.s_addr);
}

u32 srv_local_port(SRV_SOCK s)
{
    struct sockaddr_in a;
    socklen_t n = sizeof(a);

    return getsockname(s, (struct sockaddr *)&a, &n) < 0 ? 0 : ntohs(a.sin_port);
}

/* The address a datagram to `peer` leaves from: a connected UDP socket says. */
u32 srv_host_address(u32 peer)
{
    struct sockaddr_in a;
    socklen_t n = sizeof(a);
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    u32 r = 0;

    if (s < 0)
        return 0;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(9);
    a.sin_addr.s_addr = htonl(peer);
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) == 0 &&
        getsockname(s, (struct sockaddr *)&a, &n) == 0)
        r = ntohl(a.sin_addr.s_addr);
    close(s);
    return r;
}
