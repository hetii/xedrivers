/* XNetDnsLookup on lwIP's DNS client, whose servers DHCP gave. The XNDNS a caller gets is
 * one of a few kept here (no pool is taken); it says WSAEINPROGRESS until lwIP answers,
 * then 0 and the addresses, or WSAHOST_NOT_FOUND. The caller's event, if any, is set
 * then. XNetDnsRelease gives it back, at once or, still asked, when the answer comes.
 * lwIP keeps one address a name.
 */
#include "lwip/opt.h"
#include "lwip/dns.h"
#include "lwip/tcpip.h"
#include <string.h>

#include "winsock.h"

#if XE_STACK

extern s32 NtSetEvent(void *event, s32 *previous);

#define LOOKUPS 16u

static struct {
    WS_XNDNS answer;
    void *event;
    volatile u32 used, released, pending;
} lookup[LOOKUPS];

static void answered(const char *name, const ip_addr_t *ip, void *arg)
{
    u32 i = (u32)arg;

    (void)name;
    if (ip) {
        lookup[i].answer.address[0] = ip_addr_get_ip4_u32(ip);
        lookup[i].answer.count = 1;
        lookup[i].answer.status = 0;
    } else {
        lookup[i].answer.status = WSAHOST_NOT_FOUND;
        ws_stats.lookups_failed++;
    }
    lookup[i].pending = 0;
    if (lookup[i].event)
        NtSetEvent(lookup[i].event, 0);
    if (lookup[i].released)
        lookup[i].used = 0;
}

/* NetDll_XNetDnsLookup (67) */
s32 ws_XNetDnsLookup(u32 caller, const char *host, void *event, WS_XNDNS **out)
{
    ip_addr_t ip;
    err_t e;
    u32 i;

    (void)caller;
    if (!host || !out)
        return WSAEFAULT;
    if (ws_ready())
        return WSAENETDOWN;
    for (i = 0; i < LOOKUPS; i++)
        if (!__sync_lock_test_and_set(&lookup[i].used, 1))
            break;
    if (i == LOOKUPS)
        return WSAENOBUFS;
    ws_stats.lookups++;
    memset(&lookup[i].answer, 0, sizeof(lookup[i].answer));
    lookup[i].answer.status = WSAEINPROGRESS;
    lookup[i].event = event;
    lookup[i].released = 0;
    lookup[i].pending = 1;
    *out = &lookup[i].answer;
    LOCK_TCPIP_CORE();
    e = dns_gethostbyname(host, &ip, answered, (void *)i);
    UNLOCK_TCPIP_CORE();
    if (e == ERR_OK)
        answered(host, &ip, (void *)i);         /* a number, or known already */
    else if (e != ERR_INPROGRESS)
        answered(host, 0, (void *)i);
    return 0;
}

/* NetDll_XNetDnsRelease (68) */
s32 ws_XNetDnsRelease(u32 caller, WS_XNDNS *answer)
{
    u32 i;

    (void)caller;
    for (i = 0; i < LOOKUPS; i++)
        if (&lookup[i].answer == answer) {
            lookup[i].released = 1;
            mem_sync();
            if (!lookup[i].pending)
                lookup[i].used = 0;
            return 0;
        }
    return WSAEINVAL;
}

#endif /* XE_STACK */
