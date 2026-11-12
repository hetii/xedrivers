/* Start-up, clean-up, and what xnet says about the console's address and link. */
#include "lwip/opt.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <string.h>

#include "winsock.h"
#include "ipstack.h"
#include "net.h"

#if XE_STACK

void ws_to_lwip(const WS_SOCKADDR_IN *from, void *to)
{
    struct sockaddr_in *a = (struct sockaddr_in *)to;

    memset(a, 0, sizeof(*a));
    a->sin_len = sizeof(*a);
    a->sin_family = AF_INET;
    a->sin_port = from->port;
    a->sin_addr.s_addr = from->address;
}

void ws_from_lwip(const void *from, WS_SOCKADDR_IN *to)
{
    const struct sockaddr_in *a = (const struct sockaddr_in *)from;

    to->family = WS_AF_INET;
    to->port = a->sin_port;
    to->address = a->sin_addr.s_addr;
    memset(to->zero, 0, sizeof(to->zero));
}

/* WSADATA: wVersion, wHighVersion, then strings; only the first words are written. */
static void wsa_data(void *data)
{
    u16 *w = (u16 *)data;
    u8 *b = (u8 *)data;

    if (!data)
        return;
    w[0] = 0x0202;
    w[1] = 0x0202;
    b[4] = 0;                   /* szDescription */
    b[4 + 257] = 0;             /* szSystemStatus */
}

/* WSAStartup and WSACleanup come in pairs, and every system program is the same caller
 * (XNCALLER_SYSAPP): only the last WSACleanup of a game ends the game's sockets, and the
 * system's are never ended -- one program's WSACleanup took XBDM's listener with it. */
#define CALLERS 4u
static volatile u32 started[CALLERS];
#define XNCALLER_TITLE  1u

static void start(u32 caller)
{
    if (caller < CALLERS)
        __sync_fetch_and_add(&started[caller], 1);
}

static s32 stop(u32 caller)
{
    if (caller >= CALLERS || !started[caller])
        return 0;
    if (__sync_sub_and_fetch(&started[caller], 1) == 0 && caller == XNCALLER_TITLE &&
        ipstack_ready)
        ws_close_caller(caller);
    return 0;
}

/* NetDll_WSAStartup (1) */
s32 ws_WSAStartup(u32 caller, u32 version, void *data)
{
    (void)version;
    wsa_data(data);
    start(caller);
    return 0;
}

/* NetDll_WSAStartupEx (36) */
s32 ws_WSAStartupEx(u32 caller, u32 version, void *data, u32 asked)
{
    (void)version; (void)asked;
    wsa_data(data);
    start(caller);
    return 0;
}

/* NetDll_WSACleanup (2) */
s32 ws_WSACleanup(u32 caller)
{
    return stop(caller);
}

/* NetDll_XNetStartup (51), NetDll_XNetStartupEx (80), NetDll_XNetCleanup (52) */
s32 ws_XNetStartup(u32 caller, const void *params)
{
    (void)params;
    start(caller);
    return 0;
}

s32 ws_XNetStartupEx(u32 caller, const void *params, u32 version)
{
    (void)params; (void)version;
    start(caller);
    return 0;
}

s32 ws_XNetCleanup(u32 caller)
{
    return stop(caller);
}

#define XNET_GET_XNADDR_PENDING     0x0000
#define XNET_GET_XNADDR_ETHERNET    0x0002
#define XNET_GET_XNADDR_DHCP        0x0008
#define XNET_GET_XNADDR_GATEWAY     0x0020
#define XNET_GET_XNADDR_DNS         0x0040

/* NetDll_XNetGetTitleXnAddr (73): the address, and how it was had. */
u32 ws_XNetGetTitleXnAddr(u32 caller, WS_XNADDR *x)
{
    u8 mac[6];
    u32 i;

    (void)caller;
    if (!x)
        return XNET_GET_XNADDR_PENDING;
    memset(x, 0, sizeof(*x));
    if (net_uplink_mac(mac))
        for (i = 0; i < 6; i++)
            x->enet[i] = mac[i];
    if (!ipstack_ready || !ipstack_stats.ip)
        return XNET_GET_XNADDR_PENDING;
    x->ina = ipstack_stats.ip;
    return XNET_GET_XNADDR_ETHERNET | XNET_GET_XNADDR_DHCP |
           (ipstack_stats.gateway ? XNET_GET_XNADDR_GATEWAY : 0) |
           (ipstack_stats.dns ? XNET_GET_XNADDR_DNS : 0);
}

#define XNET_ETHERNET_LINK_ACTIVE       0x01
#define XNET_ETHERNET_LINK_100MBPS      0x02
#define XNET_ETHERNET_LINK_FULL_DUPLEX  0x08

/* NetDll_XNetGetEthernetLinkStatus (75) */
u32 ws_XNetGetEthernetLinkStatus(u32 caller)
{
    u8 mac[6];

    (void)caller;
    return net_uplink_mac(mac) ? XNET_ETHERNET_LINK_ACTIVE | XNET_ETHERNET_LINK_100MBPS |
                                 XNET_ETHERNET_LINK_FULL_DUPLEX : 0;
}

#endif /* XE_STACK */
